# Esp8266Base API 参考

> 版本：1.0.0  
> 平台：ESP8266 Arduino Core（仅限）  
> 前缀：`Esp8266Base` / `ESP8266BASE_`  
> 所有模块均为静态类，无需实例化。

本文件用于查 API 签名和约束。使用流程见 `docs/00_user_guide.md`；能力域细节见 Web/OTA、观测、网络、低功耗等专题文档。

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
按编译期开关初始化模块，固定顺序：Log → Sleep → Config → WiFi → Watchdog → Web → OTA。输出启动诊断日志。返回 `false` 仅表示 Config（LittleFS）初始化失败，其余模块仍会继续初始化。

```cpp
static void handle();
```
在 `loop()` 中每轮调用。推进已编译模块的非阻塞状态机，调用顺序：Config → WiFi → NTP触发 → mDNS触发 → NTP → mDNS → Web → Watchdog。

```cpp
static void setFirmwareInfo(const char* name, const char* version);
```
设置固件名称和版本号（显示在启动日志和 Web 标题中）。必须在 `begin()` 前调用。

```cpp
static void setHostname(const char* hostname);
```
设置设备 hostname，用于 AP SSID 后缀、mDNS 名称、Web 页面标题。最长 24 字符。必须在 `begin()` 前调用。

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

```cpp
typedef void (*Esp8266BaseLogHookFn)(uint8_t level,
                                     const char* tag,
                                     const char* message,
                                     const char* timestamp,
                                     const char* line);
static void setOutputHook(Esp8266BaseLogHookFn fn);
static bool enableFileSink(const char* path,
                           uint32_t maxBytes,
                           uint8_t fileLevel = ESP8266BASE_LOG_FILE_LEVEL,
                           uint8_t rotateFiles = 4);
static void disableFileSink();
static void setFileSinkLevel(uint8_t level);
static bool isFileSinkEnabled();
static const char* fileSinkPath();
static uint32_t fileSinkMaxBytes();
static uint8_t fileSinkRotateFiles();
static uint8_t fileSinkLevel();
static const char* fileSinkLevelName();
static uint32_t fileSinkSize();
static uint32_t fileSinkSegmentSize(uint8_t index);
static bool fileSinkBufferEnabled();
static uint16_t fileSinkBufferSize();
static uint16_t fileSinkBufferUsed();
static uint32_t fileSinkFlushIntervalMs();
static bool flushFileSink();
static bool clearFileSink();
static void handle();
static void beginBootSession(const char* firmware,
                             const char* version,
                             const char* bootReason,
                             uint32_t bootCount,
                             uint32_t freeHeap);
static void enableConfigAudit(bool enabled);
static void enableConfigReadAudit(bool enabled);
```

默认只输出 Serial。`setOutputHook()` 接收最终格式化日志行和拆分字段。`enableFileSink()` 启用 LittleFS 文件日志，例如 `/logs/app.log`。`rotateFiles` 支持 1-4，默认 4；当前文件超过 `maxBytes` 时会轮转为 `/logs/app.log.1`，再逐步后移到 `.2`、`.3`，最多占用约 `maxBytes * rotateFiles`。`fileLevel` 默认 `ESP8266BASE_LOG_FILE_LEVEL`，库默认 WARN；WARN/ERROR 在 file sink 启用后始终写入文件，避免关键问题被过滤。编译期 `ESP8266BASE_LOG_FILE_BUFFER_SIZE>0` 且文件等级低于 WARN 时，DEBUG/INFO 会进入低优先级缓存，达到间隔或容量后刷盘；WARN/ERROR 立即刷盘。`flushFileSink()` 用于页面读取、重启、deep sleep、OTA 成功前强制落盘。不开 file sink 时不长期占用文件日志状态；默认 WARN 时也不编译低优先级缓存。`beginBootSession()` 输出多行启动会话摘要，`bootReason` 是 ESP8266 SDK reset info 的归类结果，日志字段为 `boot_reason` 和 `boot_desc`；无法识别时输出 `unknown` / `未知启动原因`，不会输出 `undefined`。配置审计直接输出 key/value，不做任何敏感 key 特殊处理。完整说明见 `docs/07_observability.md`。

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
[2026-05-02 14:32:01][I][NTP ] log_timestamp_mode=absolute_datetime
```

首次对时成功时会先输出一条仍带 `millis()` 前缀的锚点日志：

```
[5846][I][NTP ] time_synchronized actual_time=2026-05-02 14:32:01 uptime_ms=5846 boot_time=2026-05-02 14:31:56
```

这条日志用于把对时前的相对启动时间换算成实际日期时间。

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
挂载 LittleFS。默认不会在挂载失败时自动格式化，避免损坏或临时异常时误删配置；若编译时设置 `ESP8266BASE_CFG_FORMAT_ON_FAIL=1`，挂载重试失败后会格式化并再次挂载。返回 `false` 表示文件系统不可用。

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
到达 `ESP8266BASE_CFG_DEFERRED_FLUSH_INTERVAL_MS` 间隔后最多刷 1 条 pending 写入。通过 `Esp8266Base::handle()` 自动调用。默认间隔 5000ms；设为 0 可恢复每轮最多刷 1 条的旧行为。

```cpp
static bool flush();
```
强制写完所有 pending 写入。在 deep sleep 或 `ESP.restart()` 前调用。只有全部 pending 写入成功才返回 `true`；任一写入失败时返回 `false`，失败项会保留在 deferred 队列中，等待后续 `handle()` 或下一次 `flush()` 重试。

```cpp
static bool clearAll();
```
删除所有 `/cfg_*` 配置文件，用于恢复出厂配置。函数会先丢弃 deferred 队列，避免恢复出厂前把待写数据重新写回；成功后通常应重启设备。

```cpp
static uint8_t pendingCount();
static bool isReady();
static void enableConfigAudit(bool enabled);
static void enableConfigReadAudit(bool enabled);
```
查询 deferred 队列中待写条数，及文件系统是否已就绪。

配置存储设计见 `docs/05_config_storage.md`。

配置审计默认关闭。启用写审计后，`setStr` / `setInt` / `setBool` / deferred enqueue / flush 会记录 key、old/new、changed/no_change、immediate/deferred、result。启用读审计后，`getStr` / `getInt` / `getBool` 会记录读取结果；读审计默认日志等级是 DEBUG，可用 `ESP8266BASE_CFG_READ_AUDIT_LEVEL` 调整。普通 INFO 构建不会输出大量读审计日志，避免启动和页面/API 高频读取变慢。审计日志不做任何敏感 key 特殊处理，所有值直接输出。

### 约束

- key 最长 24 字符
- string 值最长 96 字符
- deferred 队列 4 条，满则拒绝新写入
- deferred 默认每 5000ms 最多刷 1 条；同 key 高频更新只保留最新 pending 值
- 每次写入后自动 `yield()`
- OTA 期间不触发新的 deferred 写入

### 保留 key

以下 key 由库内部使用，统一使用 `eb_` 前缀，应用代码不得覆盖：

| Key | 用途 |
|-----|------|
| `eb_wifi_ssid` | WiFi STA SSID |
| `eb_wifi_pass` | WiFi STA 密码 |
| `eb_ap_pass` | AP 配网密码 |
| `eb_hostname` | 设备 hostname |
| `eb_web_user` | Web Auth 持久化用户名，覆盖默认用户名 |
| `eb_web_pass` | Web Auth 持久化密码，`/auth` 修改后写入，覆盖默认密码 |
| `eb_wdt_count` | WDT 重启累计次数 |
| `eb_wdt_pending` | 上次是否 WDT 重启 |
| `eb_boot_count` | 启动次数，无符号十进制字符串，最大 4,294,967,295，达到上限后饱和 |

### 使用示例

```cpp
// 应用自己的高频计数用 deferred
int32_t cnt = Esp8266BaseConfig::getInt("app_counter", 0) + 1;
Esp8266BaseConfig::setIntDeferred("app_counter", cnt);

// eb_boot_count 是库保留 key，由 Esp8266Base::begin() 自动维护

// 低频配置立即写
Esp8266BaseConfig::setStr(ESP8266BASE_CFG_KEY_WIFI_SSID, ssid);

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
从 Config 读取凭证并缓存到内存，启动 WiFi 状态机（非阻塞）。无凭证时直接进入 AP_CONFIG；有凭证时只按 STA 模式持续退避重连，不自动打开配置 AP。需要进入 AP 配网时，应明确清除 WiFi 凭证后重启。

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
static const char* ssid();
static int rssi();
static void macAddressTo(char* out, size_t len);
static Esp8266BaseWiFiState state();
static const char* apSSID();
```
状态查询。`ip()` 未连接时返回空字符串。`ssid()` 返回当前内存缓存中的 STA SSID。`rssi()` 仅连接后有效，未连接时返回 0。`macAddressTo()` 输出 STA MAC 地址。`apSSID()` 格式：`ESP8266-Config-XXXX`（后4位为 ChipID）。

### 默认参数

| 参数 | 默认值 |
|------|--------|
| STA 连接观察窗口 | 20000ms（`ESP8266BASE_WIFI_CONNECT_TIMEOUT`） |
| STA 稳定等待 | 150ms（`ESP8266BASE_WIFI_STA_SETTLE_MS`） |
| `WL_DISCONNECTED` 卡住恢复 | 7000ms（`ESP8266BASE_WIFI_STUCK_DISCONNECTED_MS`） |
| 快速重试间隔 | 2000ms（`ESP8266BASE_WIFI_RETRY_FAST`） |
| 快速重试次数 | 3（`ESP8266BASE_WIFI_RETRY_FAST_COUNT`） |
| 慢速重试间隔 | 60000ms（`ESP8266BASE_WIFI_RETRY_SLOW`） |
| AP SSID | `ESP8266-Config-<ChipID后4位>` |
| AP 密码 | 空（开放），可通过 key `eb_ap_pass` 配置 |

---

## 5. Esp8266BaseWeb — Web 服务

头文件：`Esp8266BaseWeb.h`

### 类型定义

```cpp
typedef void (*Esp8266BaseWebHandler)();

enum class Esp8266BaseWebHomeMode : uint8_t {
    DEFAULT_SYSTEM_HOME,
    APP_HOME_FIRST,
    FUSED_HOME
};

enum class Esp8266BaseWebSystemNavMode : uint8_t {
    TOP_NAV,
    BOTTOM_NAV,
    FOOTER_COMPACT
};

enum class Esp8266BaseWebBuiltinLabel : uint8_t {
    HOME,
    WIFI,
    OTA,
    LOGS,
    AUTH,
    REBOOT
};
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
static bool addPage(const char* path, const char* title, Esp8266BaseWebHandler handler);
static bool addApi (const char* path, Esp8266BaseWebHandler handler);
static bool addNavItem(const char* path, const char* title);
```
注册应用路由。必须在 `Esp8266Base::begin()` 之后调用。`addPage` 注册 GET 并默认加入业务导航；带 `title` 的重载用于设置导航标签。`addNavItem` 用于覆盖已注册页面的导航标签。`addApi` 同时响应 GET 和 POST。路径必须以 `/` 开头，长度小于 24 字符，只允许字母、数字、`/`、`-`、`_`、`.`；上限分别 4 / 6 个。

```cpp
static void setDeviceName(const char* name);
static void setHomePath(const char* path);
static void setHomeMode(Esp8266BaseWebHomeMode mode);
static void setSystemNavMode(Esp8266BaseWebSystemNavMode mode);
static void setBuiltinLabel(Esp8266BaseWebBuiltinLabel label, const char* title);
```
配置 Web 信息架构，通常在 `Esp8266Base::begin()` 前调用。`setDeviceName` 设置导航品牌显示名。`setHomePath` 设置业务首页路径。`setBuiltinLabel` 可覆盖 `Status/WiFi/OTA/Logs/Auth/Tools` 的导航标签，便于中文本地化。

首页模式：

| 模式 | `/` 行为 | `/esp8266base` 行为 |
|------|----------|---------------------|
| `DEFAULT_SYSTEM_HOME` | Esp8266Base 默认系统首页 | Esp8266Base 默认系统首页 |
| `APP_HOME_FIRST` | `303` 跳转到业务首页 | `303` 跳转到业务首页 |
| `FUSED_HOME` | `303` 跳转到业务首页 | 保留为融合/系统首页 |

系统导航位置：

| 模式 | 行为 |
|------|------|
| `TOP_NAV` | 基础功能入口显示在顶部导航，默认行为 |
| `BOTTOM_NAV` | 基础功能入口显示在页面内容下方 |
| `FOOTER_COMPACT` | 基础功能入口以小字号轻量链接显示在 footer，与 `Free heap` 同区 |

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
仅验证 Basic Auth，不发送 401 响应。OTA 上传在接收固件前会调用此函数；未通过认证时返回 `401 Unauthorized`。

```cpp
static void setDefaultAuth(const char* user, const char* pass);
static void setSystemInfo(const char* hostname, const char* fw, const char* ver, uint32_t bootCount);
static ESP8266WebServer& server();
static bool isRunning();
```

`setSystemInfo()` 由 `Esp8266Base::begin()` 调用，用于向内置首页传入 hostname、固件名、版本和库级 boot count。业务项目通常不需要直接调用。

`setDefaultAuth()` 只设置 Web Basic Auth 默认值，必须在 `Esp8266Base::begin()` 前调用；Web 已启动后调用会被忽略。Web Auth 密码会明文写入日志和 Config 审计，这是个人项目为了调试观察保留的设计。认证优先级为：

| 优先级 | 来源 | 说明 |
|---:|---|---|
| 1 | `ESP8266BASE_WEB_AUTH_USER/PASS` | 编译期默认值 |
| 2 | `setDefaultAuth(user, pass)` | 业务代码默认值，覆盖编译期默认值 |
| 3 | `eb_web_user` / `eb_web_pass` | 设备持久化值，覆盖所有默认值 |

内置 `/auth` 页面当前只修改 `eb_web_pass`，不修改用户名。保存成功后运行时立即使用新密码；`clearAll()` 删除配置后恢复为 `setDefaultAuth()` 或编译期默认值。

### 内置路由

| 路由 | 方法 | 功能 |
|------|------|------|
| `/` | GET | 首页；可按 `setHomeMode()` 进入业务首页 |
| `/esp8266base` | GET | 基础库系统首页；融合模式下保留为系统入口 |
| `/wifi` | GET | WiFi 设置表单 |
| `/wifi` | POST | 保存凭证并重连；成功或失败后 `303` 跳回 GET 页面，避免刷新重复提交 |
| `/auth` | GET | 修改 Web Basic Auth 密码页面（需要 Basic Auth） |
| `/auth` | POST | 校验当前密码并保存 `eb_web_pass`，成功后 `303` 回 `/auth?saved=1` |
| `/ota` | GET | OTA 上传页面（需要 Basic Auth，含上传进度） |
| `/ota` | POST | 接收固件（由 Esp8266BaseOTA 处理，强制 Basic Auth） |
| `/logs` | GET | 查看文件日志状态、大小和内容（需要 Basic Auth） |
| `/logs/clear` | POST | 清空文件日志（需要 Basic Auth，入口在 Tools 页面） |
| `/reboot` | GET | Tools 页面，包含清除文件日志和重启设备 |
| `/reboot` | POST | flush Config 后重启 |
| `/health` | GET | JSON 健康信息（heap/maxBlock/ip/uptime/wifi，无需认证） |

Web 和 OTA 完整行为见 `docs/06_web_ota.md`。

`/wifi` GET 会回显已保存 SSID/密码，密码默认隐藏，可手动切换显示。内置 WiFi、Reboot、OTA 表单都带重复提交保护；自定义页面也建议在表单 `onsubmit` 中调用 `once(this)`。

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

### OTA 行为

1. GET /ota 页面使用 Web Basic Auth；页面内用 XMLHttpRequest 上传并显示进度
2. POST /ota 在上传开始时强制验证 Basic Auth；未认证请求返回 `401 Unauthorized`
3. 上传开始：启用 `ESP8266BASE_USE_WATCHDOG=1` 时调用 `Esp8266BaseWatchdog::pause()`，然后调用 `Update.begin(ESP.getFreeSketchSpace())`，日志输出 `upload_started`
4. 上传期间：分块写入固件，每块后 `yield()`，按 10% 阶梯输出 `upload_progress`，包含 `bytes`、`request_total`、`speed`、`elapsed`
5. 上传完成：输出 `upload_finished`，启用 Watchdog 时 `resume()`，成功后输出 `upload_success`，延迟 500ms 后 `ESP.restart()`
6. 上传失败或中止：启用 Watchdog 时 `resume()`，输出 `upload_failed` 或 `upload_aborted`，包含已上传字节、`elapsed` 和 `average_speed`，返回简短错误信息

OTA 使用 `ESP.getFreeSketchSpace()` 作为写入空间，不使用 `UPDATE_SIZE_UNKNOWN`（该常量仅 ESP32 有效）。当前不计算 SHA256；接收是否成功由 `Update.write()` 和 `Update.end(true)` 的写入与镜像校验结果决定。

---

## 7. Esp8266BaseNTP — 网络对时

头文件：`Esp8266BaseNTP.h`

### 函数

```cpp
static bool begin();
```
配置 NTP 服务器和时区，启动系统 SNTP 客户端，并启用库内主动 UDP NTP 查询。由 `Esp8266Base::handle()` 在 WiFi 首次连接时自动调用。

```cpp
static void handle();
```
检查同步状态。系统 SNTP 或库内主动 UDP NTP 任一路径成功后，都会设置系统时间，并自动调用 `Esp8266BaseLog::setTimeProvider()` 切换日志时间格式。首次同步会记录实际时间、启动后毫秒数和推算出的本次启动时间，便于换算同步前的日志。WiFi 断开后主入口会调用 `reset()` 释放 UDP socket；WiFi 重连后会重新 `begin()`。

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
| 主动 UDP 查询 | 未同步时自动轮询 3 个服务器，单次等待 3s |

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
启用 ESP8266 SDK 管理的 `WIFI_MODEM_SLEEP`。CPU 继续运行，STA 连接保持，射频由 SDK 按 DTIM 间隔休眠。

```cpp
static void wakeModem();
```
恢复 `WIFI_NONE_SLEEP`。

```cpp
static void deepSleep(uint32_t sleepSec);
```
进入 deep sleep（单位：秒，最大 3600）。内部自动执行：

1. 启用 `ESP8266BASE_USE_WATCHDOG=1` 时调用 `Esp8266BaseWatchdog::pause()`
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
`handle()` 检查主循环活性，超时时写入重启记录后执行 `ESP.restart()`。`feed()` 重置计时器。两者均由 `Esp8266Base::handle()` 自动调用，顺序为先检查、再在本轮完成后喂狗。长阻塞操作中需手动调用 `feed()`。

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
WDT 重启累计次数查询与清零。超时路径先写 RTC 标记并重启，下一次正常启动阶段补写 Config key `eb_wdt_count`；`eb_wdt_pending` 仅用于兼容旧固件残留标记。

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
| `ESP8266BASE_LOG_FILE_LEVEL` | `2` | 文件日志默认等级，默认 WARN |
| `ESP8266BASE_LOG_FILE_BUFFER_SIZE` | `WARN 以下为 512，否则 0` | DEBUG/INFO 文件日志低优先级缓存；最大 512B |
| `ESP8266BASE_LOG_FILE_FLUSH_INTERVAL_MS` | `2000` | 低优先级文件日志缓存刷盘间隔 |
| `ESP8266BASE_CFG_READ_AUDIT_LEVEL` | `0` | 配置读审计等级，默认 DEBUG |
| `ESP8266BASE_USE_WEB` | `1` | 编译 Web 管理页和 Web 扩展 API |
| `ESP8266BASE_USE_OTA` | `0` | 编译 OTA；要求 `ESP8266BASE_USE_WEB=1` |
| `ESP8266BASE_USE_NTP` | `0` | 编译 NTP 对时 |
| `ESP8266BASE_USE_MDNS` | `1` | 编译 mDNS |
| `ESP8266BASE_USE_SLEEP` | `1` | 编译 Sleep |
| `ESP8266BASE_USE_WATCHDOG` | `1` | 编译 Watchdog |
| `ESP8266BASE_WEB_MAX_APP_PAGES` | `4` | 应用页面最大注册数 |
| `ESP8266BASE_WEB_MAX_APP_APIS` | `6` | 应用 API 最大注册数 |
| `ESP8266BASE_WEB_AUTH_USER` | `"admin"` | Basic Auth 编译期默认用户名 |
| `ESP8266BASE_WEB_AUTH_PASS` | `"admin"` | Basic Auth 编译期默认密码 |
| `ESP8266BASE_CFG_FORMAT_ON_FAIL` | `0` | LittleFS 挂载失败时是否自动格式化 |
| `ESP8266BASE_NTP_TIMEZONE` | `28800` | 时区偏移秒（UTC+8 = 8×3600） |
| `ESP8266BASE_NTP_SYNC_INTERVAL` | `3600` | NTP 重新同步间隔（秒） |
| `ESP8266BASE_NTP_SERVER_1..3` | 中国镜像服务器 | NTP 服务器 |
| `ESP8266BASE_WDT_TIMEOUT_MS` | `2500` | 看门狗超时毫秒 |
| `ESP8266BASE_CFG_DEFERRED_SIZE` | `4` | deferred 写入队列长度 |
| `ESP8266BASE_CFG_DEFERRED_FLUSH_INTERVAL_MS` | `5000` | deferred 写入最小刷盘间隔 ms |
| `ESP8266BASE_CFG_KEY_MAX` | `24` | Config key 最大字符数 |
| `ESP8266BASE_CFG_STR_MAX` | `96` | Config string value 最大字节数 |
| `ESP8266BASE_WIFI_CONNECT_TIMEOUT` | `20000` | WiFi STA 单次连接观察窗口 ms |
| `ESP8266BASE_WIFI_STA_SETTLE_MS` | `150` | 切换 STA/断开旧状态后，调用 `WiFi.begin()` 前的稳定等待 ms |
| `ESP8266BASE_WIFI_STUCK_DISCONNECTED_MS` | `7000` | STA 连接中持续 `WL_DISCONNECTED` 无进展时，提前重启本轮连接 |
| `ESP8266BASE_WIFI_RETRY_FAST` | `2000` | WiFi 快速重试间隔 ms |
| `ESP8266BASE_WIFI_RETRY_FAST_COUNT` | `3` | WiFi 快速重试次数 |
| `ESP8266BASE_WIFI_RETRY_SLOW` | `60000` | WiFi 慢速重试间隔 ms |
| `ESP8266BASE_SLEEP_MAX_DEEP_SEC` | `3600` | deepSleep 最大秒数上限 |
