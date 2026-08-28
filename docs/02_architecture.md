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
│     │ │     │ │      │  │  Web / OTA / MQTT   │
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
  ├── Esp8266BaseMQTT       （可选；依赖 WiFi + NTP 就绪；业务通过固定函数指针接入）
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
3. Esp8266BaseFilesystem::begin()   — `USE_FILESYSTEM=1` 时仅挂载 LittleFS；默认不自动格式化
4. Esp8266BaseConfig::begin()       — `USE_CONFIG=1` 时初始化 KV/deferred 状态
5. Esp8266BaseFileLog::begin()      — `USE_FILELOG=1` 时读取模式并注册日志 sink
6. Esp8266BaseWiFi::begin()         — 读取凭证并缓存，启动状态机（非阻塞）
7. Esp8266BaseWatchdog::begin()     — `ESP8266BASE_USE_WATCHDOG=1` 时
8. Esp8266BaseWeb::begin()          — `ESP8266BASE_USE_WEB=1` 时（注册内置路由，开始监听）
9. Esp8266BaseOTA::begin()          — `ESP8266BASE_USE_OTA=1` 时（要求 Web，注册 POST /ota）
10. Esp8266BaseMQTT::begin()        — `ESP8266BASE_USE_MQTT=1` 时准备固定区与 TLS，不立即建连
11. Esp8266Base::logDiagnostics()   — 输出启动诊断日志
```

> NTP 和 mDNS 不在 `begin()` 中初始化，而是在 `handle()` 检测到 WiFi 连接时自动触发。

---

## 五、handle() 调度顺序

`Esp8266Base::handle()` 在 `loop()` 中每轮调用一次：

```text
1. Esp8266BaseConfig::handle()      — 刷新 deferred 写入（每轮最多 1 条）
2. Esp8266BaseFileLog::handle()     — 低优先级文件日志缓存到期刷新
3. Esp8266BaseWiFi::handle()        — 状态机推进
4. NTP 触发检测                      — WiFi 已连接且 NTP 未启动：调用 Esp8266BaseNTP::begin()
5. mDNS 触发检测                     — WiFi 已连接且 mDNS 未启动：调用 Esp8266BaseMDNS::begin()
                                       WiFi 掉线时：重置 mDNS 状态，等待重连后重启
6. Esp8266BaseNTP::handle()         — `ESP8266BASE_USE_NTP=1` 时
7. Esp8266BaseMDNS::handle()        — `ESP8266BASE_USE_MDNS=1` 时
8. Esp8266BaseWeb::handle()         — `ESP8266BASE_USE_WEB=1` 时，请求前后喂库级 WDT
9. Esp8266BaseMQTT::handle()        — `ESP8266BASE_USE_MQTT=1` 时；WiFi/NTP 门控后推进连接
10. Esp8266BaseWatchdog::handle()   — `ESP8266BASE_USE_WATCHDOG=1` 时
   Esp8266BaseWatchdog::feed()      — 本轮完成后喂狗
```

库自有状态机不得忙等。Web 固定先于 MQTT 推进，使已经到达的本地请求不会先等待本轮 DNS/TCP/TLS 建连。MQTT 收发按每轮固定字节预算推进；ESP8266 同步安全传输的 connect 单次尝试仍可能阻塞到连接超时，业务可在执行器运行期间关闭新建连接门禁，有界退避保证失败后不会立即重试。

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

`ESP8266BASE_PROFILE_MQTT_TERMINAL=0`（默认）使用完整路由：

```text
ESP8266WebServer（端口 80）
  ├── 内置路由（固定）
  │     GET  /
  │     GET  /esp8266base
  │     GET  /wifi
  │     POST /wifi       ──► Basic Auth + 每次启动表单令牌，保留 SSID/密码首尾空格
  │     GET  /auth
  │     POST /auth       ──► Basic Auth + 每次启动表单令牌，校验当前密码并保存 eb_web_pass
  │     GET  /ota
  │     POST /ota        ──► Esp8266BaseOTA 处理（强制 Basic Auth）
  │     GET  /logs
  │     GET  /system     ──► System 页面：Hostname、WiFi、Auth、OTA、FileLog、清日志、重启入口
  │     POST /system/filelog ─► 保存 FileLog 模式（入口在 System 页面）
  │     POST /logs/clear ──► 清空文件日志（入口在 System 页面）
  │     POST /system/hostname ──► 保存 eb_hostname，重启生效
  │     GET/POST /api/system/hostname ──► hostname JSON API
  │     POST /reboot      ──► flush Config 后重启
  │     GET  /health
  │
  └── 应用路由（静态数组）
        _pages[0..3]   GET handler   （最多 4 个）
        _apis [0..5]   GET+POST      （最多 6 个）
```

`ESP8266BASE_PROFILE_MQTT_TERMINAL=1` 只注册以下基础路由：

```text
GET       /           （AP_CONFIG 时 303 /wifi；STA 时 303 ESP8266BASE_TERMINAL_HOME_PATH，默认 /health）
GET/POST /wifi
GET/POST /auth
GET       /health
POST      /ota       （仅启用 OTA；由 Esp8266BaseOTA 独立注册）
应用页面/API          （仅按显式容量编译；容量为 0 时没有对应数组）
```

MQTT_TERMINAL 不注册完整系统首页、System、Logs、hostname、reboot 或 `GET /ota`，也不生成完整系统导航。`POST /ota` 不依赖上传页面，因此脚本和 curl 上传仍可工作。

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

业务容量为 0 时，`_pages` 或 `_apis` 数组由预处理直接排除；完整模式默认容量仍为 4+6。

---

## 八、MQTT 与 OTA 单向编排

依赖方向固定为 `Esp8266Base → Esp8266BaseMQTT` 与 `Esp8266BaseOTA → Esp8266BaseMQTT`。MQTT 模块不调用 OTA、Web 或业务代码；业务只注册 connected/disconnected/message/SUBACK/publish acknowledgement/client error 普通函数指针。connected callback 每次成功连接都会触发，重新订阅由业务完成。

业务握手失败通过 `requestReconnect()` 设置延迟处理标志，握手成功通过 `markConnectionReady()` 确认稳定。CONNACK 不重置 `_retryDelay`，连续握手失败由断线回调沿同一 `_scheduleRetry()` 推进到 60s；ready 确认才恢复初始 2s。强制断开的异步 callback 若已进入 `BACKOFF`，`handle()` 不会重复 schedule，因此单次失败只推进一次退避。

普通 MQTT 状态为 `UNCONFIGURED → WAITING_WIFI → WAITING_TIME → BACKOFF/CONNECTING → CONNECTED`。断线按 2s、4s、8s、16s、32s、60s 有界指数退避；WiFi 或 NTP 不就绪时回到门控状态。底层同步 BearSSL connect 在 ESP8266 上可能阻塞到单次网络超时，但外围不会忙循环。

断线完成后，`cleanSession=true` 会清空两个固定出站槽，再通知业务并安排下一次连接；旧连接周期证据不能进入新会话。`cleanSession=false` 仅保留未确认 QoS1 PUBLISH，重置为未发送并置 DUP；SUBSCRIBE 与 QoS0 不跨连接。

业务可用 `setConnectAttemptsEnabled(false)` 暂停后续 DNS/TCP/TLS 新建连接，以保护执行器的本地单调截止。门禁不拆除已建立会话，已连接客户端仍执行 `loop()`；恢复为 `true` 后沿既有退避时间继续。该接口只调整传输调度，不改变 MQTT 协议契约。

MQTT 传输直接使用 `BearSSL::WiFiClientSecure` 实现所需的 MQTT 3.1.1 子集：CONNECT/CONNACK、SUBSCRIBE/SUBACK、QoS0/1 PUBLISH/PUBACK、PING 和 DISCONNECT。两个固定出站槽按 `CRITICAL → EVIDENCE → STATE`、同优先级 FIFO 选择，但任何时刻只有一个 QoS1 包在途；因此业务顺序提交 runtime、overview 时，overview 必须等待 runtime 的匹配 PUBACK。入站 topic 固定 128B，payload 以 256B 窗口分块，单包上限 768B。容量耗尽或越界立即返回明确错误，不分配 heap。

受控下线是 `CONNECTED → SHUTDOWN_WAIT_ACK → SHUTDOWN_DISCONNECTING → PAUSED` 的单向状态机。`beginShutdown(topic, payload)` 把 retained QoS1 最终消息复制到固定槽；进入后立即禁止新的 publish/subscribe/message/reconnect/readiness。只有固定槽精确匹配最终 packetId 的 PUBACK 才发送正常 MQTT DISCONNECT，随后 flush socket、释放 TLS 并得到 `SUCCESS`；不匹配 PUBACK 不推进。入队失败、连接丢失或 PUBACK 超时保留明确结果和暂停门禁，绝不主动用异常 close 冒充正常成功。业务显式 `resumeAfterShutdown()` 才重新允许连接。

OTA 顺序为：业务 prepare 完成执行器安全停机；若存在活动会话则调用 `beginShutdown()` → `pauseForOTA()` 有界等待匹配 PUBACK 和正常 MQTT DISCONNECT/TLS 释放 → `Update.begin()`。MQTT 未配置、未 begin 或底层已完全 disconnected 时，`pauseForOTA()` 直接进入 `PAUSED` 并允许 OTA，同时保留 `NOT_CONNECTED` 或原失败结果；这只表示无活动传输，不表示 shutdown 消息成功。CONNECTED、CONNECTING 或 transport 尚未释放且未完成受控下线时拒绝。失败路径先恢复 Watchdog 和 MQTT 连接许可，成功路径保持暂停并重启。

---

## 九、Config 存储架构

文件系统布局（LittleFS）：

```text
/
├── cfg_eb_wifi_ssid    → "IOTHOME"
├── cfg_eb_wifi_pass    → "secret123"
├── cfg_eb_hostname     → "sensor-node-01"
├── cfg_eb_wdt_count    → "3"
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

## 十、全局 RAM 预算

全局静态 RAM 预算以 `docs/04_memory_budget.md` 为唯一权威来源。本文只描述架构关系，避免维护两份预算表导致数值漂移。

---

## 十一、关键不做项

| 项目 | 原因 |
|------|------|
| HAL 抽象层 | 增加层级，无 RAM 收益，单平台不需要 |
| 虚函数 | 虚表指针增加 RAM，阻止内联优化 |
| std::function | 每个对象额外 ~16-24B heap，不可控 |
| STL 容器 | heap 碎片化，RAM 不可预测 |
| 通用事件总线 | 增加框架复杂度和 RAM 常驻状态 |
| 复杂状态页 | 大 HTML 缓冲耗 RAM |
| 异步 Web | ESPAsyncWebServer RAM 占用更大 |
