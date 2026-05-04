# Esp8266Base 项目评估报告

- **评估日期**：2026-05-03
- **被评对象**：Esp8266Base v1.0.0（README/docs/src/examples/platformio.ini/library.json）
- **平台范围**：ESP8266 Arduino Core，无 ESP32 兼容分支
- **评估视角**：嵌入式系统架构师 / Arduino 库维护者 / 量产固件负责人

---

## 0. 总体结论

**当前状态：不建议直接以 1.0.0 对外发布。代码整体结构清晰、模块边界合理、RAM 预算可接受，但存在两个 P0 级量产阻塞问题（OTA 未鉴权、密码全程明文落盘日志），以及若干 P1 级影响生产可靠性的问题（WDT-pending 误判、LittleFS 误格式化、NTP 不重启、状态机死代码、文档与代码不一致）。**

P0 修复后，库可作为「内网/可信网络」轻量基础库小范围发布；要作为可信公网设备的固件底座，还需补 P1 项。整体技术取舍（静态类、固定数组、PROGMEM、deferred 写）方向正确，没有过度设计的根本性问题，主要是边界/兜底/安全的细节缺失。

**总体可继续实施，但 P0 必须修复后再打 1.0.0 tag。**

---

## 1. P0 阻塞（发布前必须修）

### P0-1 OTA POST 路由完全未鉴权（高危安全漏洞）

- **位置**：`src/Esp8266BaseOTA.cpp:18-32`（`begin()`），`src/Esp8266BaseOTA.cpp:38-56`（`_handleUploadComplete`），`src/Esp8266BaseOTA.cpp:62-103`（`_handleUploadChunk`）
- **代码佐证**：
  ```cpp
  Esp8266BaseWeb::server().on("/ota", HTTP_POST,
      _handleUploadComplete,
      _handleUploadChunk
  );
  ```
  两个回调内均未调用 `Esp8266BaseWeb::checkAuth()` / `verifyAuth()`。
- **文档佐证**（`docs/03_api_reference.md` §6 / `docs/06_web_extension.md` §六）已明确写明「POST /ota 不做额外 Basic Auth 校验」，说明这是有意设计。
- **风险**：任何能访问设备 80 端口的客户端都可以 `curl -F firmware=@evil.bin http://device/ota` 上传任意固件并触发 `ESP.restart()`。等于把刷机口完全开放。在家用 / 办公 / 工厂网络中，意味着任何被入侵的同网段终端都能批量刷机或植入恶意固件。属于量产灾难级。
- **建议修复**：
  - 在 `_handleUploadChunk` 的 `UPLOAD_FILE_START` 分支首先调用 `Esp8266BaseWeb::verifyAuth()`，未通过则 `Update.end()` + `Update.abort()`、置内部 reject 标志，并在 `_handleUploadComplete` 中返回 401。
  - 或在 `Esp8266BaseWeb` 中提供 `checkAuthForUpload()`：先解析 `Authorization` 头（请求体到达前），未通过则直接 `_server.requestAuthentication()` + `_server.client().stop()`。
  - 同步把 `docs/03_api_reference.md` §6 / `docs/06_web_extension.md` §六中「POST /ota 不做额外 Basic Auth 校验」的描述改为「上传 POST 强制 Basic Auth」。
- **附注**：注释里写的「避免 multipart upload 被二次认证拒绝」是误解 — `ESP8266WebServer::authenticate()` 本身不消费 body，可以在 START 阶段调用。

### P0-2 WiFi 密码全程明文写入串口日志

- **位置**（命中点）：
  - `src/Esp8266BaseWiFi.cpp:39`：`loaded_saved_wifi_credentials … password=%s`
  - `src/Esp8266BaseWiFi.cpp:99`：`saving_wifi_credentials … password=%s`
  - `src/Esp8266BaseWiFi.cpp:177`：`station_connecting … password=%s`
  - `src/Esp8266BaseWeb.cpp:_handleWiFiPost` 内：`wifi_credentials_form_submitted … password=%s`
  - `examples/basic_wifi/src/main.cpp:55`、`67`：`password_buffer_updated … password=%s` / `serial_connect_command … password=%s`
- **风险**：
  1. 量产设备日志常被运维 / 客户 / 第三方测试机抓走；明文密码会污染日志、违反基本最小信息暴露原则。
  2. 默认日志等级 `LOG_LEVEL=1` (Info) 即输出，无法靠等级过滤。
  3. 多条日志重复打同一密码（启动、连接、保存），增大泄露面。
- **建议修复**：
  - 删除所有 `password=%s`，最多保留 `password_length=%u` 与 `password_set=yes/no`。
  - `_handleWiFiPost` 的 form 日志移除明文密码。
  - 示例 `basic_wifi` 同步删改。
  - 文档（如有 sample log）一并更新。
- **附带建议**：SSID 保留是合理的（需要现场判断接错网），但可以在 docs 里明示「日志中会包含 SSID 但不包含密码」。

---

## 2. P1 发布前必须修

### P1-1 LittleFS 挂载失败时无条件 `format()`，可能销毁现场配置

- **位置**：`src/Esp8266BaseConfig.cpp:24-37`
  ```cpp
  if (!LittleFS.begin()) {
      ESP8266BASE_LOG_W("Cfg ", "LittleFS mount failed, formatting...");
      LittleFS.format();
      ...
  }
  ```
- **风险**：第一次烧录裸芯片是合理的，但**正常运行中 mount 偶发失败**（电源抖动、写入中掉电、SDK bug）会被等价处理为「格式化」，导致 WiFi 凭证、计数、应用配置全部丢失。量产场景里这意味着「设备一次电源抖动 → 全部回到 AP 配网 → 客户体验灾难」。
- **建议修复**：
  - 引入 `cfg_fs_marker` 文件 + 启动计数：仅在「之前从未成功挂载过」（marker 不存在或本次启动是首次烧录）时才允许 format；正常运行中 mount 失败应只 `LOG_E`，让上层禁用 Config 相关功能，不擦盘。
  - 或在 format 前再做一次 `LittleFS.begin()` retry + delay，避免一次性失败立即抹库。
  - 文档 `docs/05_config_storage.md §七` 同步说明改动。

### P1-2 `wdt_pending` 标志会把「WDT 重启后断电再上电」误判为 WDT 重启

- **位置**：`src/Esp8266BaseWatchdog.cpp:25-33`
  ```cpp
  int pending = Esp8266BaseConfig::getInt("wdt_pending");
  if (pending) { _wasWdtReset = true; ... }
  ```
- **场景**：A) WDT 触发 → 写入 `wdt_pending=1` → `ESP.restart()`；B) 但若用户在 A 之后立即拔电；C) 下次任意原因（power-on、ext reset）上电时 `wdt_pending` 仍为 1，会被误判为 WDT 重启 → `resetCount` 不再增加但 `wasWatchdogReset()` 报 true，遥测/日志会归错原因。
- **建议修复**：在 `wdt_pending` 判定后，**结合 `Esp8266BaseSleep::wakeReason()` 是否真为 `wdt-reset`** 才认定。两者交叉验证后再写回 0。
- **附注**：`Esp8266BaseSleep::begin()` 已经先于 Watchdog::begin() 执行，此交叉判断在初始化顺序上是可行的。

### P1-3 NTP 在 WiFi 掉线后不会重启 / `_ntpWasTriggered` 永不复位

- **位置**：`src/Esp8266Base.cpp:101-108`
  ```cpp
  if (!_ntpWasTriggered && wifiNow) {
      Esp8266BaseNTP::begin();
      _ntpWasTriggered = true;
  }
  ```
- **现状**：`_ntpWasTriggered` 只在 begin() 时初始化为 false，handle 里只有「false → true」的赋值，从无 reset。WiFi 掉线再连后，NTP `begin()` 不会再调用，库内主动 UDP NTP 也不会重新初始化（mDNS 倒是有这个 reset 逻辑，与 NTP 不一致）。
- **风险**：
  - 设备首次连上「假联网热点」（路由器在线但 NAT 未通）时 SNTP 永久失败；后续切到真互联网热点也不会重新触发 begin。
  - 长期运行设备如果路由换 DNS，UDP NTP 路径里缓存的 IP 不会刷新（_manualIp 也没失效逻辑）。
- **建议修复**：参考 mDNS 模式增加 `else if (_ntpWasTriggered && !wifiNow) { _ntpWasTriggered = false; }`；NTP 内部的 `_synced` 也应在 begin() 外保留，但 `_pollManual` 状态机要在 WiFi 重连时重置。
- **附注**：此项与 P1-7 的「NTP 文档与代码不符」相关。

### P1-4 `ESP8266BASE_WIFI_CONNECT_TIMEOUT` 后无 AP 兜底，与文档/AGENTS.md 描述不一致

- **位置**：
  - 文档 `CLAUDE.md` / `AGENTS.md`：「Credentials saved but connection fails after timeout → falls back to AP config mode」。
  - 实际代码 `src/Esp8266BaseWiFi.cpp:78-87`（`_scheduleRetry`）：超时只是退避到下一次重试，**永远不会**进入 `AP_CONFIG`，与文档严重不符。
  - `docs/03_api_reference.md §4` 又改口为「有凭证时只按 STA 模式持续退避重连，不自动打开配置 AP」。
- **风险**：
  - 文档读者会以为「连不上必有 AP 兜底」，量产现场把设备搬去新地点（旧热点不存在）会**永远联不上、无任何配置入口**，只能拆机或硬复位。
  - CLAUDE.md / AGENTS.md / docs 三套说法不一致，维护方向错乱。
- **建议修复**（任选其一并统一文档）：
  - **方案 A（推荐量产）**：N 次（如 5 次）连续失败后，自动开启 `WIFI_AP_STA` 让用户可重新配网，但保留 STA 重试。
  - **方案 B（保持现状）**：把 CLAUDE.md / AGENTS.md 里的 fallback 描述改掉，强制现场必须按按键 / 串口 `clear` 回 AP 模式；并在 `full_demo` 里把按键文档放更显眼位置。

### P1-5 默认 AP 无密码 + 默认 Web 密码弱

- **位置**：
  - `src/Esp8266BaseWiFi.cpp:202-217`：AP 没读到 `ap_pass` 即 open。
  - `src/Esp8266BaseWeb.h:33-37`：`ESP8266BASE_WEB_AUTH_PASS` 默认 `"esp8266"`。
- **风险**：开盒即用 = 内网任何人都能：
  1. 连 AP（无密码）；
  2. 用默认密码 `admin/esp8266` 登入控制台；
  3. 改 WiFi、刷固件（叠加 P0-1 更糟）。
- **建议修复**：
  - AP 默认密码：基于 ChipID 派生 8 位（如 `ESP-XXXXXXXX`），首页提示用户改。
  - Web Auth：默认密码改为基于 ChipID 派生（不可预测），强制首次登录提示修改。
  - 至少在 README 「明确不支持」附近再加一节「量产前必改项」。

### P1-6 `Esp8266BaseConfig::_cfgBuf` / `Esp8266BaseWeb::_wb` 共享缓冲非重入，但调用路径已经存在「同时使用」风险

- **位置**：
  - `src/Esp8266BaseConfig.cpp:11-12`：`static char _cfgBuf[97]`，注释「与 Web 模块共用，不重复计入预算」 — 但代码上**两边没有任何互斥**。
  - `src/Esp8266BaseWeb.cpp:96`：`static char _wb[160]`。
  - 触发场景：Web handler 内调用 `Esp8266BaseConfig::getStr()` / `setStr()`（如 `_handleWiFiGet`、`_handleWiFiPost`）—— `setStr` 内部用 `_cfgBuf` 做写前比较，正好夹在「sendChunk(用 _wb)」之间，但因为非真正多线程，单回路里只要顺序正确就 OK。
- **风险**：当前路径都是顺序的，不会出问题；但**注释撒谎**「共用」给后续维护者埋雷 —— 任何人添一个「先 snprintf 进 _wb，调用 Config，再用 _wb」的 handler 立即踩坑。`_cfgBuf` 与 `_wb` 实际是两个独立 buffer，但被描述为「共享」。
- **建议修复**：
  - 把 `Esp8266BaseConfig.cpp:11-12` 的注释改为「Config 模块私有，约 97B」。
  - 在 `docs/04_memory_budget.md` 重新核算（当前 NTP / Config / Web 模块预算与代码不完全一致，见 P2-1）。
  - 写一行 `// non-reentrant — do not use across nested calls` 给 `_wb`。

### P1-7 文档与代码版本号 / 默认值多处不一致

- **冲突列表**：
  | 项 | 文档/位置 | 代码/位置 | 实际默认 |
  |---|---|---|---|
  | `ESP8266BASE_WIFI_CONNECT_TIMEOUT` | CLAUDE.md / AGENTS.md：`15000` | `Esp8266BaseWiFi.h:18`：`20000` / README / `docs/03 §11`：`20000` | `20000` |
  | WiFi fast retry | CLAUDE.md：`15s` | 代码 / README / docs：`5s` | `5000` |
  | `ESP8266BASE_WDT_TIMEOUT_MS` | `Esp8266BaseWatchdog.h:18`：`2000` | platformio.ini / docs：`2500` | 由 build_flags 决定 |
  | AP fallback 行为 | CLAUDE.md / AGENTS.md：会回 AP | 代码 / `docs/03 §4`：不会回 AP | 不会 |
  | `library.json` `repository.url` | `https://github.com/yourname/Esp8266Base` | — | 占位符未替换 |
- **风险**：用户按 README 编译可以跑，按 CLAUDE.md 调参会得到错误预期；library.json 把库发到 PlatformIO Registry 时是无效 URL。
- **建议修复**：
  - 确定 single source of truth：以 `docs/03_api_reference.md §11` 为准；CLAUDE.md / AGENTS.md 与之同步或简化为「见 docs/03」。
  - `Esp8266BaseWatchdog.h:18` 把 `2000` 改 `2500`，与文档一致。
  - `library.json` 替换为真实 git URL。

### P1-8 NTP UDP socket 在同步成功后未关闭（次要资源浪费 + 端口占用）

- **位置**：`src/Esp8266BaseNTP.cpp:54`：`_ntpUdp.begin(NTP_LOCAL_PORT)` —— 之后即使 `_synced=true`，UDP 仍占 socket，`_pollManual()` 在 `_synced` 后通过 `_pollManual` 路径 early-return，但 socket 没释放。
- **风险**：
  - ESP8266 lwip 默认有限 UDP socket 数（实际 ~4），长期占着 1 个。
  - 与未来扩展的 mDNS query / DNS resolver 抢资源时调试难度高。
- **建议修复**：`_finishSync()` 中 `_ntpUdp.stop()`；只在 `_synced=false` 路径下保持 listening。

### P1-9 Web `setTitle()` 设置了 `_titleBuf`，但 HTML 头部根本没 `<title>`，且 `_handleRoot` 把它当 `<h2>` 用 — 接口语义混乱

- **位置**：
  - `src/Esp8266BaseWeb.cpp:166-168`：`setTitle()` 写入 48B buffer。
  - `src/Esp8266BaseWeb.cpp:269-273`（`_handleRoot`）：`snprintf(_wb, ..., "<h2>%s</h2>", _titleBuf);`
  - `src/Esp8266BaseWeb.cpp:WEB_HEAD`：`<head>...<style>...</style><script>...</script></head>` —— **没有 `<title>` 标签**。
- **风险**：
  - 浏览器 tab 名为空 / 显示 IP。
  - 名为 `setTitle` 的 API 实际只影响首页 H2，开发者期望落空。
- **建议修复**：在 `WEB_HEAD` 模板里加 `<title>` 占位 + sendChunk 注入 `_titleBuf`，或把 `setTitle` 改名 `setHomeHeading()` 并在 docs 里明示。

### P1-10 `Esp8266BaseWeb::handle` 慢请求日志对应用 handler 显示「unknown」

- **位置**：`src/Esp8266BaseWeb.cpp:120-131`
- **现状**：`_activeUri` / `_activeMethod` 仅在 `_markRequest()` 内写入，而内置 handler / `sendHeader` 调用了它，但**应用自定义 handler（如 `handleSensorPage`）若不调 sendHeader 直接 server.send 就不会 mark**。所有 `addApi` 注册的纯 JSON API（`server.send` 即返回）几乎都触发 `(unknown)` 日志，定位极困难。
- **风险**：量产排查问题时，慢请求日志失真。
- **建议修复**：在 `addPage` / `addApi` 注册时包一层 trampoline `static void _appWrap(handler) { _markRequest(); handler(); }`，或在 handle() 入口直接读 `_server.uri()` / `_server.method()`（handle() 已知该次请求）。

### P1-11 `Esp8266BaseSleep::modemSleep` 与 `Esp8266BaseWiFi::_startSTA` 互相覆盖

- **位置**：
  - `src/Esp8266BaseWiFi.cpp:174`：`WiFi.setSleepMode(WIFI_NONE_SLEEP);`（每次 `_startSTA` 都强制设回 NONE）
  - `src/Esp8266BaseSleep.cpp:62`：`WiFi.setSleepMode(WIFI_MODEM_SLEEP);` 设置 `_modemSleeping=true`，但 WiFi 状态机不知情。
- **风险**：开了 modem sleep 之后任何 WiFi 重连（断网恢复、`connect()` 调用）都会**静默关闭** modem sleep，但 `_modemSleeping` 仍为 true。后续 `wakeModem()` 不做事，应用以为还在省电模式实际不是。功耗 / 电池设备出 bug 极难定位。
- **建议修复**：
  - `_startSTA` 内不强制 `WIFI_NONE_SLEEP`，改为「读取 Sleep 模块的 `_modemSleeping` 决定」。
  - 或者删除 `WiFi.setSleepMode` 调用，让 Sleep 模块独享 modem sleep 控制权。

### P1-12 `Esp8266BaseConfig::clearAll()` 先 `flush()` 再删除文件 = 浪费一次写入 + 潜在断电窗口

- **位置**：`src/Esp8266BaseConfig.cpp:226-243`
- **风险**：clearAll 的 use case 是「恢复出厂」，flush 写完立刻 remove 是无意义的 Flash 损耗；若在 flush ↔ remove 之间断电，磁盘留着「上次未提交的脏数据」反而把恢复出厂语义破坏掉。
- **建议修复**：直接清空 `_deferred[]`（不 flush），再批量删除 `/cfg_*`。

---

## 3. P2 建议修

### P2-1 `docs/04_memory_budget.md` 与代码实际预算偏差

- NTP 实际 = ~24B 状态 + `WiFiUDP _ntpUdp`（lwip socket 控制块在 heap，~30-100B 不固定）+ `IPAddress _manualIp`(4B) + `_timeStr` 静态 buf 20B。文档说「<= 224B」涵盖不到 UDP 内部 + sntp_set_servers 的 strdup（注意：`configTime` 内部对 server name 做 `strdup`，每个 server 名 ~16B 在 heap）。建议在表格里注明「不含 lwip / sntp 内部 heap」。
- WiFi 文档「<= 384B」实际 ~204B（28+16+64+64+18+padding），有 ~180B 空间余量，OK 但表中 `_apSSID(28B) + _ip(16B) + _staSSID(64B) + _staPass(64B)` 加状态/计时器 18B = 190B，文档里 384B 偏宽松。
- 建议把表加一列「实测」，每次发版用 `nm --size-sort` 校对。

### P2-2 `Esp8266BaseWiFi::_startSTA(..., bool keepAP)` 的 `keepAP` 是死代码

- **位置**：`src/Esp8266BaseWiFi.h:78` 默认参数 `keepAP=false`，全文搜索从未传 true。
- **建议**：删除参数，逻辑内联，简化阅读。

### P2-3 `Esp8266BaseConfig::clearAll()` 不区分「内置保留 key」与「应用 key」

- **现象**：clearAll 把 `wifi_ssid` / `wifi_pass` / `wdt_count` 全部删掉。
- **建议**：提供 `clearApp()`（只删非保留 key）与 `clearAll()`（全删）两套；`docs/05` 加示例。

### P2-4 NTP 服务器表硬编码 `% 3`，扩展易错

- **位置**：`src/Esp8266BaseNTP.cpp:128, 147, 158`：`_manualServer = (_manualServer + 1) % 3;`
- **建议**：定义 `static const uint8_t NTP_SERVER_COUNT = sizeof(NTP_SERVERS)/sizeof(NTP_SERVERS[0]);` 取代硬编码 3。

### P2-5 默认 NTP 服务器全部为中国镜像

- **位置**：`src/Esp8266BaseNTP.cpp:24-27`
- **建议**：保留中国镜像作为默认（与项目目标用户匹配），但文档显式说明「如部署海外，推荐改 `pool.ntp.org`，方法：自定义 `Esp8266BaseNTP::begin()` 之前调用 `configTime` 覆盖」。最好把 server 列表也开放为编译宏 `ESP8266BASE_NTP_SERVER_1..3`。

### P2-6 `Esp8266BaseWatchdog::handle()` 触发后做 3 次 Flash 写入

- **位置**：`src/Esp8266BaseWatchdog.cpp:54-60`
  ```cpp
  Esp8266BaseConfig::setInt("wdt_count",   ...);
  Esp8266BaseConfig::setInt("wdt_pending", 1);
  Esp8266BaseConfig::flush();   // setInt 都是立即写，flush 是 no-op
  ```
- **建议**：`flush()` 可删除（setInt 不进 deferred queue）。当前 1 次 reset 写 2 文件已足够；进一步可合并为单文件 JSON `wdt_state` 减少 Flash 磨损。

### P2-7 `_handleNotFound` 不区分 method，也无 auth；可被用于 fingerprint

- **现状**：永远返回 `404 "Not found"`，泄露的信息少 OK；但仍可被探测设备类型（结合 `/health`）。
- **建议**：可选地把 `/health` 限制为 STA mode（AP mode 关闭）以减少配网阶段的暴露。

### P2-8 `Esp8266BaseWeb::server()` 公开了底层 `ESP8266WebServer&` —— 抽象漏洞

- **风险**：应用代码可绕过 `_pages` / `_apis` 上限直接 `server().on(...)`，使路由数和 RAM 预算失真。RAM 预算文档无法约束。
- **建议**：保留 `server()` 用于 `arg()` / `client()`（确实需要），但在 docs 里明确写「禁止用 `server().on(...)` 注册路由，必须走 addPage/addApi」。

### P2-9 `examples/full_demo/src/main.cpp::sendConfigTable()` 直接遍历 LittleFS，绕过 Config API

- **位置**：full_demo 主文件
- **风险**：其他用户照抄会重复实现遍历 + 字符串处理。
- **建议**：在 `Esp8266BaseConfig` 增加 `forEach(callback)` 或 `nextKey(...)`；handler 内只调 callback。

### P2-10 `Esp8266BaseWeb::sendContent_P` 栈缓冲 256B + sendHeader/sendFooter 嵌套调用栈

- **位置**：`src/Esp8266BaseWeb.cpp:255`（`char buf[256]`）
- **风险**：ESP8266 默认任务栈 4KB；sendContent_P 在 sendHeader / sendFooter 中重入调用一次，应用 handler 中也再调一次，单帧 256B 累加 + printf args 等可能逼近 1KB。
- **建议**：缩到 128B（HTML 片段大多 < 128B 即可），降低栈压力。

### P2-11 `mDNS` 在 WiFi 掉线时只重置标志，未调 `MDNS.end()`

- **位置**：`src/Esp8266Base.cpp:114-117`
- **风险**：依赖 `MDNS.begin()` 内部「重复调用安全」，行为依赖 ESP8266mDNS 库实现，不显式调 `end()` 是 fragile assumption。
- **建议**：reset 路径加 `MDNS.end();`（若库支持）。

### P2-12 `Esp8266BaseWeb` 表单 SSID/Password 长度后端无强校验

- **位置**：`_handleWiFiPost` 用 `char ssid[64]` / `char pass[64]` 截断；HTML form `maxlength=32` / `maxlength=64`。
- **风险**：客户端绕过 maxlength 提交超长字符串 → 服务端只是被截断，无明确错误。
- **建议**：长度超界时显式 redirect 到 `?error=ssid_too_long` / `pass_too_long`。

### P2-13 `Esp8266BaseLog::_timeStr()` 每条日志都重新 `strftime` + 静态 buf 返回

- **位置**：`src/Esp8266BaseNTP.cpp:215-222`
- **风险**：高频日志（如 LOG_D 全开）在 strftime 上的 CPU 浪费可见；非线程安全（虽然 ESP8266 单核，安全）但若未来引入 ISR 内 log 会踩雷。
- **建议**：缓存「秒级」结果，秒未变就返回上次字符串；或文档明示「禁止在 ISR 中调用 LOG_*」。

### P2-14 README 与 docs 各处重复描述相同的「编译宏表」、「目录结构」

- **现象**：README、docs/01、docs/03 §11 三份编译宏表；不一致风险（已被 P1-7 命中）。
- **建议**：README 只放精简版 + 链接到 `docs/03 §11`；docs/01 删除或改成 ToC。

### P2-15 `examples/basic_wifi/platformio.ini [env:nodemcuv2]` 未声明 `board_build.ldscript`

- **风险**：与 esp12f 分区不一致，OTA 大小可能不同。
- **建议**：统一显式声明分区脚本，否则注释清楚原因。

### P2-16 `Esp8266BaseSleep::deepSleep(0)` 行为含糊

- **现状**：传 0 表示永久睡眠到 RST；clamp 不影响（0 < 3600）；但 docs/03 §9 没明示。
- **建议**：docs 写明「sleepSec=0 = 永久睡眠，需外部 RST 唤醒」。

### P2-17 `library.json` 缺 `examples` 字段，PlatformIO Registry 不会自动列示例

- **建议**：补 `"examples": ["examples/*/src/*.cpp"]`。

### P2-18 `Esp8266BaseConfig::setStr` 写后无 verify

- **现状**：File 关闭后无读回校验。
- **风险**：极少数 LittleFS 写失败但 close 成功的边缘场景不会被发现。
- **建议**（推测，需 soak test 确认）：可选 `setStrVerified()` 用于 wifi_ssid / wifi_pass 关键写。

### P2-19 OTA `Update.begin()` 失败后未 reject 后续 chunk

- **位置**：`src/Esp8266BaseOTA.cpp:75-79`
- **现状**：begin 失败仅 LOG_E，不设标志；后续 `UPLOAD_FILE_WRITE` 仍会 `Update.write(...)` 持续失败并 LOG_E spam。
- **建议**：begin 失败设 `_inProgress=false`，UPLOAD_FILE_WRITE 入口 early-return；UPLOAD_FILE_END 也跳过 `Update.end(true)`。

### P2-20 `CLAUDE.md` 与 `AGENTS.md` 内容几乎逐字相同

- **风险**：维护两份文档，长期必然漂移（已经在 P1-7 体现）。
- **建议**：保留一份（例如 `CLAUDE.md`），另一份 1 行链接。

---

## 4. 架构与模块边界（横向小结）

正确的部分：
- 模块依赖严格单向（`Esp8266Base` → 各模块），无循环。
- 初始化顺序与 handle 调度顺序在文档与代码中一致（除已列出的 NTP 重启问题）。
- 编译期开关 `ESP8266BASE_USE_*` 全部用 `#if` 真正裁剪 cpp 文件，无 dead-call；`ESP8266BASE_USE_OTA && !USE_WEB` 有 `#error` 校验。
- 静态类 + 函数指针 + 静态数组的取舍贯彻彻底，无 `std::function`、无 STL。

需要警惕的点（不一定是 bug，但要文档化）：
- Web 模块直接暴露 `server()`、Config 模块暴露内部缓冲注释（"共享"），是潜在维护性裂缝（见 P1-6 / P2-8）。
- `Esp8266Base.cpp` 内 NTP/mDNS 触发逻辑写死在主入口（不在各自模块内 self-trigger），以后再加新「需 WiFi 后初始化的模块」会需要改主入口 — 设计上 OK，明示出来即可。

---

## 5. 测试与验证现状

**现状**：仓库内**完全没有自动化测试**（无 `test/`、无 GitHub Actions、无 PlatformIO `test_dir`），只有 5 个示例工程供编译验证。文档 `docs/04` 给了「示例构建资源参考」表（Flash/RAM 数字），但没有脚本固化这一比对。

**建议补充（按优先级）**：

1. **CI 编译矩阵**（最低要求）：
   - GitHub Actions：`pio ci --board esp12e --board nodemcuv2` 跑 5 个示例 + 库根目录。
   - 至少跑 `pio check` (cppcheck) 抓未初始化变量。

2. **必须实机验证清单**（量产 1.0.0 前）：
   - **OTA 鉴权**：用 curl 不带 `-u` 直接 POST /ota，确认被拒（修 P0-1 后）。
   - **LittleFS 误格式化**：拔电触发 mount 失败模拟（很难，可手工腐蚀分区），验证不再无条件 format（修 P1-1 后）。
   - **WDT pending 误判**：人工 `freeze` → 等 WDT 重启 → 立即拔电 → 上电，看日志归因是否被纠正（修 P1-2 后）。
   - **WiFi 重连后 NTP / mDNS**：连接后断网 5 分钟再恢复，看 NTP 是否再同步、mDNS 是否再广播（修 P1-3 后）。
   - **OTA 期间 WDT**：上传 > 200KB 固件，全程不被 WDT 重启。
   - **Modem sleep + WiFi 重连**：开 modemSleep → 主动断网 → 重连，看实际 SDK sleep mode 是否仍为 modem sleep（修 P1-11 后）。
   - **Soak test 24h**：full_demo 工程开 LOG_D，curl `/api/demo` 每 5 秒一次，监控 free_heap 不持续下降（验证无堆碎片化）、`maxBlock` 稳定。
   - **AP 配网 + 提交后浏览器刷新**：确认 303 重定向真的避免重复提交。

3. **可选静态检查**：
   - `arm-none-eabi-size .pio/build/esp12f/firmware.elf` 出 RAM/Flash 数字与 docs/04 表比对，超出阈值即 fail。
   - `nm --size-sort .pio/build/esp12f/firmware.elf | grep Esp8266Base` 抓最大静态符号，校对 RAM 预算。

---

## 6. 最小修复清单（按上线顺序）

1. **【P0-1】** OTA POST 加 Basic Auth（`Esp8266BaseOTA::_handleUploadChunk` 的 `UPLOAD_FILE_START` 入口调用 `Esp8266BaseWeb::verifyAuth()`，未通过则 `Update.end()/abort` 并在完成回调返 401）。
2. **【P0-2】** 删除所有 `password=%s` 日志（5 处源文件 + 1 处示例）。
3. **【P1-1】** Config 挂载失败兜底改为「retry 一次再 format」并加 marker 文件，避免运行中误格式化。
4. **【P1-2】** Watchdog `_wasWdtReset` 用 `wdt_pending && wakeReason()=="wdt-reset"` 双条件判定。
5. **【P1-3】** 主入口在 WiFi 掉线时 reset `_ntpWasTriggered`，与 mDNS 对齐。
6. **【P1-4】** 决策并统一 AP fallback 行为（推荐：N 次失败后切 AP_STA），同步 CLAUDE.md / AGENTS.md / docs。
7. **【P1-5】** AP 默认密码、Web 默认密码改为 ChipID 派生；首页提示用户改。
8. **【P1-6】** 修正 `_cfgBuf` 的「共享」注释 + Web `_wb` 加 non-reentrant 标注。
9. **【P1-7】** 同步默认值（`WIFI_CONNECT_TIMEOUT=20000`、`WDT_TIMEOUT_MS=2500`、`WIFI_RETRY_FAST=5000`），更正 library.json URL。
10. **【P1-8】** NTP 同步成功后 `_ntpUdp.stop()`。
11. **【P1-9】** Web 模板加 `<title>`，与 setTitle 语义对齐（或重命名 setHomeHeading）。
12. **【P1-10】** Web 慢请求日志增加 app handler trampoline，使 URI 不再 unknown。
13. **【P1-11】** Sleep / WiFi 不再互覆盖 `setSleepMode`。
14. **【P1-12】** clearAll 内移除多余 flush。
15. **【P2 全部】** 在 1.0.x 后续 patch 版本里收掉。

---

## 7. 建议验证命令

修完 P0/P1 后，用以下命令组合验证（顺序执行）：

```bash
# 1. 全示例本地编译（确保没有破坏构建）
for d in examples/*/; do (cd "$d" && pio run -e esp12f) || exit 1; done
(cd examples/basic_wifi && pio run -e nodemcuv2)

# 2. 静态检查（cppcheck）
pio check --severity=high --skip-packages

# 3. 烧录 full_demo
cd examples/full_demo && pio run -e esp12f --target upload

# 4. 抓串口（CLAUDE.md 建议方式）
python3 -c "
import serial, time
s = serial.Serial('/dev/cu.usbserial-120', 115200, timeout=1)
end = time.time() + 600  # 10 分钟 soak
while time.time() < end:
    line = s.readline()
    if line: print(line.decode('utf-8', errors='replace'), end='', flush=True)
"

# 5. OTA 鉴权回归（修 P0-1 后必须返回 401）
curl -i -F firmware=@.pio/build/esp12f/firmware.bin http://esp-demo.local/ota
# 期望：HTTP/1.1 401 Unauthorized

# 6. OTA 鉴权通过路径
curl -i -u admin:<new-pass> -F firmware=@.pio/build/esp12f/firmware.bin \
     http://esp-demo.local/ota
# 期望：HTTP/1.1 200 + "OK: Firmware updated. Rebooting..."

# 7. 健康检查 + heap 基线
curl -s http://esp-demo.local/health | python3 -m json.tool
# 期望：heap >= 24KB（Web idle 场景）

# 8. WiFi 重连 → NTP 重新同步（修 P1-3 后）
# 手工断 AP，观察 [I][NTP ] ntp_client_started 第二次打印

# 9. 24h soak
# 持续 curl /api/demo，观察 heap 不下降、maxBlock 稳定
while true; do
  curl -s -u admin:<pass> http://esp-demo.local/api/demo \
    | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['heap'], d['maxblk'])"
  sleep 5
done
```

---

## 8. 推测项明示

以下结论为「推测」，需要实机验证：

- **P1-1**：「正常运行中 mount 偶发失败」是工程经验上的可能性，未在本项目代码库实际复现。验证方法：故意烧错 LittleFS 镜像或在写入中拔电。
- **P1-2**：WDT pending + power-on 误判路径未实测。验证：人工触发 freeze + 立即拔电序列。
- **P1-8**：UDP socket 长期占用对其他模块的影响是经验推测，ESP8266 lwip socket 配额需以 `LWIP_UDP_SOCKETS` 实测为准。
- **P2-13**：strftime 性能影响在低日志频率下可忽略，需 LOG_D 全开 + 高频路径上验证才能成为问题。
- **P2-10**：栈接近 1KB 是估算。可用 `uxTaskGetStackHighWaterMark`-style 工具实测（Arduino Core 上手段有限）。

---

**评估完成。**
