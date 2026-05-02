# Esp8266Base 架构设计

> 版本：1.0.0  
> 平台：ESP8266 Arduino Core（仅限）  
> 前缀：`Esp8266Base` / `ESP8266BASE_`

---

## 一、设计原则

1. **RAM 优先**：任何设计决策以 RAM 余量为第一约束。
2. **无深层继承**：不使用虚函数（`virtual`），不做复杂多态。
3. **静态类**：所有模块以静态方法暴露接口，不需要实例化。
4. **无动态分配**：模块内部不使用 `new` / `malloc`，不使用 STL 容器。
5. **单平台专注**：代码中无 `#ifdef ESP32` 或任何跨平台分支。

---

## 二、整体分层

```text
┌─────────────────────────────────────────────────┐
│                  App（用户代码）                  │
│  setup() / loop() / custom web handlers          │
└───────────────────┬─────────────────────────────┘
                    │ Esp8266Base::begin() / handle()
┌───────────────────▼─────────────────────────────┐
│              Esp8266Base 主入口                   │
│  初始化协调 / handle() 分发 / 启动诊断            │
└──┬────────┬────────┬──────────┬─────────────────┘
   │        │        │          │
┌──▼──┐ ┌──▼──┐ ┌───▼──┐  ┌───▼────────────────┐
│ Log │ │ Cfg │ │ WiFi │  │   Network Layer     │
│     │ │     │ │      │  │  Web / OTA          │
└─────┘ └─────┘ └──────┘  │  NTP / mDNS         │
                           └─────────────────────┘
┌───────────────────────────────────────────────┐
│              Runtime Layer                     │
│           Sleep / Watchdog                     │
└───────────────────────────────────────────────┘
```

---

## 三、模块依赖关系

```text
Esp8266Base（主入口）
  ├── Esp8266BaseLog        （无依赖，最先初始化）
  ├── Esp8266BaseSleep      （无依赖，早于 Config 检测唤醒原因）
  ├── Esp8266BaseConfig     （依赖 LittleFS）
  ├── Esp8266BaseWiFi       （依赖 Config 读取凭证；凭证缓存在内存，重连不再读 Flash）
  ├── Esp8266BaseWeb        （begin() 在 WiFi 之后；始终监听，AP 模式也可访问）
  │     └── Esp8266BaseOTA  （依赖 Web server 已启动；GET 页面使用 Web Basic Auth）
  ├── Esp8266BaseNTP        （WiFi 连接后由 handle() 触发 begin()，成功后回调 Log 切换时间）
  ├── Esp8266BaseMDNS       （WiFi 连接后由 handle() 触发 begin()；WiFi 掉线后重置，重连自动重启）
  ├── Esp8266BaseSleep      （deepSleep 前调用 Config::flush() 和 Watchdog::pause()）
  └── Esp8266BaseWatchdog   （OTA 期间自动 pause/resume；handle() 最后执行，确保其他模块已运行）
```

依赖方向：单向，下层不反向依赖上层。

---

## 四、begin() 初始化顺序

`Esp8266Base::begin()` 内部严格按以下顺序执行：

```text
1. Esp8266BaseLog::begin()          — 最先，保证后续日志可输出
2. Esp8266BaseSleep::begin()        — 读取唤醒原因（须在 Config 前）
3. Esp8266BaseConfig::begin()       — 挂载 LittleFS；首次挂载失败时自动 format 后重试
4. Esp8266BaseWiFi::begin()         — 读取凭证并缓存，启动状态机（非阻塞）
5. Esp8266BaseWatchdog::begin()     — `ESP8266BASE_USE_WATCHDOG=1` 时
6. Esp8266BaseWeb::begin()          — `ESP8266BASE_USE_WEB=1` 时（注册内置路由，开始监听）
7. Esp8266BaseOTA::begin()          — `ESP8266BASE_USE_OTA=1` 时（要求 Web，注册 POST /ota）
8. Esp8266Base::logDiagnostics()    — 输出启动诊断日志
```

> NTP 和 mDNS 不在 `begin()` 中初始化，而是在 `handle()` 检测到 WiFi 连接时自动触发。

---

## 五、handle() 调度顺序

`Esp8266Base::handle()` 在 `loop()` 中每轮调用一次：

```text
1. Esp8266BaseConfig::handle()      — 刷新 deferred 写入（每轮最多 1 条）
2. Esp8266BaseWiFi::handle()        — 状态机推进
3. NTP 触发检测                      — WiFi 已连接且 NTP 未启动：调用 Esp8266BaseNTP::begin()
4. mDNS 触发检测                     — WiFi 已连接且 mDNS 未启动：调用 Esp8266BaseMDNS::begin()
                                       WiFi 掉线时：重置 mDNS 状态，等待重连后重启
5. Esp8266BaseNTP::handle()         — `ESP8266BASE_USE_NTP=1` 时
6. Esp8266BaseMDNS::handle()        — `ESP8266BASE_USE_MDNS=1` 时
7. Esp8266BaseWeb::handle()         — `ESP8266BASE_USE_WEB=1` 时，请求前后喂库级 WDT
8. Esp8266BaseWatchdog::handle()    — `ESP8266BASE_USE_WATCHDOG=1` 时
   Esp8266BaseWatchdog::feed()      — 本轮完成后喂狗
```

每个 handle() 不得有长阻塞。长操作必须分步推进或在操作中定期 `yield()`。

---

## 六、WiFi 状态机

```text
          ┌──────┐       有凭证       ┌────────────┐
     ───► │ IDLE │ ─────────────────► │ CONNECTING │
          └──┬───┘                    └─────┬──────┘
             │ 无凭证                       │ connected
             ▼                              ▼
          ┌───────────┐  connect()    ┌───────────┐
          │ AP_CONFIG │ ────────────► │ CONNECTED │
          └───────────┘               └─────┬─────┘
                                           │ WiFi 掉线
                                           ▼
                                     ┌────────────┐
                                     │ CONNECTING │
                                     │ 前 3 次 15s │
                                     │ 之后 60s    │
                                     └────────────┘
```

重连使用内存中缓存的凭证（`_staSSID` / `_staPass`），从不在重连路径上读 Flash。若启动时已有凭证但暂时连不上，设备保持纯 STA 模式并持续退避重连，不自动打开配置 AP。只有无凭证，或用户明确清除凭证后重启，才进入 `AP_CONFIG`。

---

## 七、Web 路由架构

```text
ESP8266WebServer（端口 80）
  ├── 内置路由（固定）
  │     GET  /
  │     GET  /wifi
  │     POST /wifi
  │     GET  /ota
  │     POST /ota        ──► Esp8266BaseOTA 处理（页面登录后上传，不额外校验）
  │     GET  /reboot      ──► 确认页
  │     POST /reboot      ──► flush Config 后重启
  │     GET  /health
  │
  └── 应用路由（静态数组）
        _pages[0..3]   GET handler   （最多 4 个）
        _apis [0..5]   GET+POST      （最多 6 个）
```

路由表内存结构：

```cpp
struct AppRoute {
    char                  path[24];   // 24B
    Esp8266BaseWebHandler handler;    // 4B
    bool                  isApi;      // 1B（+ 3B padding）
};                                    // 32B per entry
// 总计：4×32 + 6×32 = 320B
```

---

## 八、Config 存储架构

文件系统布局（LittleFS）：

```text
/
├── cfg_wifi_ssid    → "IOTHOME"
├── cfg_wifi_pass    → "secret123"
├── cfg_wdt_count    → "3"
├── cfg_wdt_pending  → "0"
└── cfg_<key>        → <value>
```

路径格式：`/cfg_<key>`，key 最大 24 字符。

Deferred 队列（静态，4 条）：

```cpp
struct DeferredEntry {
    char    key[24];
    int32_t intVal;
    bool    boolVal;
    uint8_t type;   // 1=int32, 2=bool
    bool    used;
};
static DeferredEntry _deferred[ESP8266BASE_CFG_DEFERRED_SIZE];
```

`handle()` 每轮最多写 1 条；`flush()` 强制写完所有 pending（deep sleep / restart 前调用）。

---

## 九、全局 RAM 预算汇总

| 模块 | 静态 RAM 预算 | 包含内容 |
|------|--------------|----------|
| Esp8266BaseLog | <= 160B | level(1B) + fn ptr(4B)；格式缓冲 128B 在栈上 |
| Esp8266BaseConfig | <= 512B | deferred 队列 4×34B + 状态标志 + 读写缓冲 97B |
| Esp8266BaseWiFi | <= 384B | 状态/计时器 + _apSSID(28B) + _ip(16B) + _staSSID/Pass(128B) |
| Esp8266BaseWeb（路由表） | <= 512B | AppRoute 数组 320B + auth(48B) + title(48B) + 状态 |
| Esp8266BaseOTA | <= 128B | _inProgress(1B) |
| Esp8266BaseNTP | <= 160B | 同步状态 + 时区偏移(4B) + 计时器(8B) |
| Esp8266BaseMDNS | <= 96B | 运行状态 |
| Esp8266BaseSleep | <= 48B | _wakeReason ptr(4B) + 标志(2B) |
| Esp8266BaseWatchdog | <= 96B | timeout(4B) + 计时器(8B) + pause(1B) + count(4B) |
| **库总计（自有）** | **< 2.5KB** | 不含 Arduino SDK 内部开销 |

Arduino SDK 内部开销（参考值，不可控）：

| 组件 | 估算 RAM |
|------|---------|
| WiFi SDK | ~12-15KB |
| ESP8266WebServer | ~4-6KB（含请求缓冲） |
| LittleFS | ~2-3KB |
| Arduino Core | ~3-4KB |

---

## 十、关键不做项

| 项目 | 原因 |
|------|------|
| HAL 抽象层 | 增加层级，无 RAM 收益，单平台不需要 |
| 虚函数 | 虚表指针增加 RAM，阻止内联优化 |
| std::function | 每个对象额外 ~16-24B heap，不可控 |
| STL 容器 | heap 碎片化，RAM 不可预测 |
| 事件总线 | 动态订阅需要动态分配或大静态表 |
| FileLog | 高频 Flash 写增加阻塞风险 |
| 复杂状态页 | 大 HTML 缓冲耗 RAM |
| 异步 Web | ESPAsyncWebServer RAM 占用更大 |
