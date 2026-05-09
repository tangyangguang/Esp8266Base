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
  ├── Esp8266BaseNTP        （WiFi 连接后由 handle() 触发 begin()；SNTP + 主动 UDP NTP 双路径对时）
  ├── Esp8266BaseMDNS       （WiFi 连接后由 handle() 触发 begin()；WiFi 掉线后重置，重连自动重启）
  ├── Esp8266BaseSleep      （deepSleep 前调用 Config::flush()；启用 Watchdog 时先 pause）
  └── Esp8266BaseWatchdog   （启用时 OTA 期间自动 pause/resume；handle() 最后执行，确保其他模块已运行）
```

依赖方向：单向，下层不反向依赖上层。

---

## 四、begin() 初始化顺序

`Esp8266Base::begin()` 内部严格按以下顺序执行：

```text
1. Esp8266BaseLog::begin()          — 最先，保证后续日志可输出
2. Esp8266BaseSleep::begin()        — 读取唤醒原因（须在 Config 前）
3. Esp8266BaseConfig::begin()       — 挂载 LittleFS；默认不自动格式化
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
2. Esp8266BaseLog::handle()         — 低优先级文件日志缓存到期刷新
3. Esp8266BaseWiFi::handle()        — 状态机推进
4. NTP 触发检测                      — WiFi 已连接且 NTP 未启动：调用 Esp8266BaseNTP::begin()
5. mDNS 触发检测                     — WiFi 已连接且 mDNS 未启动：调用 Esp8266BaseMDNS::begin()
                                       WiFi 掉线时：重置 mDNS 状态，等待重连后重启
6. Esp8266BaseNTP::handle()         — `ESP8266BASE_USE_NTP=1` 时
7. Esp8266BaseMDNS::handle()        — `ESP8266BASE_USE_MDNS=1` 时
8. Esp8266BaseWeb::handle()         — `ESP8266BASE_USE_WEB=1` 时，请求前后喂库级 WDT
9. Esp8266BaseWatchdog::handle()    — `ESP8266BASE_USE_WATCHDOG=1` 时
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
                                     │ 前 3 次 2s  │
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
  │     GET  /esp8266base
  │     GET  /wifi
  │     POST /wifi
  │     GET  /auth
  │     POST /auth       ──► 校验当前密码并保存 eb_web_pass
  │     GET  /ota
  │     POST /ota        ──► Esp8266BaseOTA 处理（强制 Basic Auth）
  │     GET  /logs
  │     POST /logs/clear ──► 清空文件日志
  │     GET  /reboot      ──► 确认页
  │     POST /reboot      ──► flush Config 后重启
  │     GET  /health
  │
  └── 应用路由（静态数组）
        _pages[0..3]   GET handler   （最多 4 个）
        _apis [0..5]   GET+POST      （最多 6 个）
```

应用路由路径必须以 `/` 开头，长度小于 24 字符，并且只允许字母、数字、`/`、`-`、`_`、`.`。内置导航和系统首页会对应用提供的路径、标题和日志路径做 HTML 输出转义。

路由表内存结构：

```cpp
struct AppRoute {
    char                  path[24];   // 24B
    char                  title[18];  // 18B
    Esp8266BaseWebHandler handler;    // 4B
    bool                  isApi;      // 1B
    bool                  showInNav;  // 1B
};                                    // 48B per entry
// 总计：4×48 + 6×48 = 480B
```

---

## 八、Config 存储架构

文件系统布局（LittleFS）：

```text
/
├── cfg_eb_wifi_ssid    → "IOTHOME"
├── cfg_eb_wifi_pass    → "secret123"
├── cfg_eb_wdt_count    → "3"
├── cfg_eb_wdt_pending  → "0"
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

`handle()` 到达 `ESP8266BASE_CFG_DEFERRED_FLUSH_INTERVAL_MS` 后最多写 1 条；同 key 高频更新只覆盖内存 pending 值。`flush()` 强制写完所有 pending（deep sleep / restart 前调用），只有全部 pending 写入成功才返回 `true`；失败项会保留在队列中，避免静默丢失 deferred 配置。

---

## 九、全局 RAM 预算

全局静态 RAM 预算以 `docs/04_memory_budget.md` 为唯一权威来源。本文只描述架构关系，避免维护两份预算表导致数值漂移。
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
| 通用事件总线 | 增加框架复杂度和 RAM 常驻状态 |
| 复杂状态页 | 大 HTML 缓冲耗 RAM |
| 异步 Web | ESPAsyncWebServer RAM 占用更大 |
