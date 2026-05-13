# Esp8266Base 问题与隐患评估

> 评估日期：2026-05-09
> 项目版本：1.0.0
> 评估范围：`src/`、`docs/`、`examples/`、`tools/`、根目录 `README.md` / `AGENTS.md` / `CHANGELOG.md` / `library.json` / `platformio.ini` / `partitions/`
> 本文件**只列出问题与隐患**，不做修改建议落地，也不评分。
> 按严重程度分级；优先级以"对生产固件的潜在影响"评估。

> **历史归档说明（2026-05-12）**：本文件是早期评估归档，部分条目已修复或已按当前项目口径关闭。当前口径以 `AGENTS.md`、`README.md`、`docs/` 和最新代码为准；WiFi/Web/Auth/Config 明文输出与存储、默认 `admin/admin`、`eb_boot_count` 启动立即写入、`/health` 免认证均为设计，不再按本文件旧条目视为 bug。

---

## A. 安全 / 信息泄露

### A1（中–高）`/auth` 处理函数把 Web 密码全部明文写入 Serial 和文件日志
位置：`src/Esp8266BaseWeb.cpp:868-892`

- 失败分支：`web_password_change_rejected ... current=%s expected=%s`（line 869）、`new=%s confirm=%s`（line 875）
- 成功分支：`web_password_updated password=%s ...`（line 890）
- `_loadPersistedAuth()` 同样输出 `web_auth_loaded user=... password=...`（line 529）
- `AGENTS.md` / `docs/07_observability.md` 中"明文输出"的豁免**只覆盖 WiFi 密码和 Config 审计**，不包含 Web Auth 密码。
- 即便依据"明文友好"理念，把当前密码、新密码、确认密码（含输错值）一并写入 LittleFS 文件日志等于把改密历史持久化到设备闪存。被授权访问 `/logs` 的任何人都能拿到全部 Web 密码，明显超出 WiFi 调试场景的边界。

### A2（中）改密失败时把"输错的当前密码"和"目标密码"一并落盘
- 与 A1 同位置。错误路径里输出明文 `current=` / `confirm=` 让攻击者能从日志里抓取用户输错的值（很可能与系统其他密码相关）。

### A3（低）`_handleWiFiGet` 把保存的 WiFi 密码作为 HTML 表单 value 渲染
位置：`src/Esp8266BaseWeb.cpp:777-784`

- 任何登录到管理页的人都能在 `/wifi` GET 页面打开"显示密码"按钮看到明文。
- 文档里没有显式声明"`/wifi` 表单回显密码是设计意图"（只有日志层面声明）。这是与 Basic Auth 强度等价的可见面，但应在文档中明示。

### A4（低）默认凭证 `admin / esp8266` 在多处文档中作为示例直接出现
- 业务项目复制粘贴时极容易把默认密码部署到生产。
- `AGENTS.md` 和 `README.md` 没有强制要求覆盖；`tools/check_static.sh` 也没有阻止使用默认密码的 release 编译。

---

## B. 功能性 Bug / 可能错误

### B1（中–高）NTP 把栈上 buffer 当 `configTime` 服务器名指针
位置：`src/Esp8266BaseNTP.cpp:49-54`

```cpp
char s1[20], s2[20], s3[20];
strncpy_P(s1, NTP_S1, sizeof(s1) - 1); ...
configTime(ESP8266BASE_NTP_TIMEZONE, 0, s1, s2, s3);
```

- ESP8266 Arduino core 的 `configTime` 最终调用 lwip `sntp_setservername`，**该函数不复制字符串，只保存指针**。
- `NTP::begin()` 返回后 `s1/s2/s3` 所在栈帧会被复用，后续 SNTP 重新查询拿到的就是垃圾数据。
- 表现可能是：首次同步靠手写 UDP 路径成功，之后每小时 SDK 自动续期会查不到 NTP 服务器。即便目前测试看起来正常，也是 UB。
- 模块自己其实已经定义了持久化的 `static const char NTP_S1[] PROGMEM`，把数据复制到栈再传的写法本身就矛盾。

### B2（中）OTA `UPLOAD_FILE_END` 路径上若 `_handleUploadComplete` 不被触发，watchdog 永远停在 paused 状态
位置：`src/Esp8266BaseOTA.cpp:97-99`、`src/Esp8266BaseOTA.cpp:125-135`

- `pause()` 在 `UPLOAD_FILE_START` 调用，`resume()` 只在 `_handleUploadComplete()`（POST 完成响应阶段）和 `UPLOAD_FILE_ABORTED` 调用。
- 如果客户端在 `UPLOAD_FILE_END` 后直接断连，ESP8266WebServer 不一定会再调用 onComplete 回调；若仅触发 `_handleUploadChunk(UPLOAD_FILE_END)` 后客户端断开，`_inProgress=true`、`Watchdog::resume()` 没被调用，主循环此后永远不再喂狗。
- 即便实际 SDK 在客户端断开时通常会调度 onComplete，把 resume 仅放在 onComplete 单点是脆弱设计：一旦上层框架行为变化或 onComplete 抛错，就是"OTA 后看门狗死路"。

### B3（中）`Esp8266BaseConfig::clearAll` 边遍历 `Dir.next()` 边删除文件
位置：`src/Esp8266BaseConfig.cpp:415-434`

- LittleFS 的 `Dir` 迭代器在迭代过程中删除当前条目可能导致跳过下一条目（取决于底层实现）。
- `clearAll()` 是"恢复出厂"的关键路径，部分残留 `cfg_*` 文件意味着出厂复位后可能把旧的 WiFi/Web 凭证带回来。

### B4（中）`_setStrInternal` 写入失败时的回滚不完整
位置：`src/Esp8266BaseConfig.cpp:144-150`

```cpp
if (!LittleFS.rename(tmp, path)) {
    if (hadOld && LittleFS.exists(bak) && !LittleFS.exists(path)) {
        LittleFS.rename(bak, path);
    }
    LittleFS.remove(tmp);
    ...
}
```

- 边界场景：如果 `rename(path, bak)` 成功（旧值已变 bak），但 `rename(tmp, path)` 失败，并且 `LittleFS.exists(path)` 此时返回 true（少见但理论可能），回滚就被跳过，bak 留在文件系统里。
- `LittleFS.remove(bak)` 仅在成功 rename 后执行（line 152），这条 fallback 路径里 `LittleFS.remove(tmp)` 后 bak 文件残留。下次读会成功返回 path 的内容，但 bak 永远残留，并参与 B5 的恢复逻辑。

### B5（中）`_readRaw` 空文件恢复逻辑会破坏"故意为空"的值
位置：`src/Esp8266BaseConfig.cpp:85-94`

```cpp
if (n == 0 && LittleFS.exists(bak)) {
    LittleFS.remove(path);
    if (LittleFS.rename(bak, path)) { ... }
}
```

- 比如 `Esp8266BaseWiFi::clearCredentials()` 调 `setStr(eb_wifi_ssid, "")`，正常会写一个 0 字节的 cfg 文件。
- 正常路径下 `_writeRaw` 写入完成会删除 bak（line 152），不会触发该逻辑；但如果"清空 + 写新值失败"的混合场景留下了 bak（参 B4），下次读会把"空"恢复成"旧 SSID"。
- `clearCredentials` 又只调 `setStr` 而不强制 flush 文件系统，这种竞争窗口存在。

### B6（中）`_loadAndIncrementBootCount` 立即落盘，每次启动都对 LittleFS 写一次
位置：`src/Esp8266Base.cpp:3-30`

- 用 `setStr` 立即写。日均启动几十次的设备一年下来会对 LittleFS 同一文件做上万次写入。
- 若设备频繁因业务上电（电源管理、deep sleep、WDT 重启），会显著加速磨损。
- 设计上更适合启动时仅入 deferred 队列、停机时由 `flush()` 落盘。

### B7（低–中）WiFi `_retryCount` 是 `uint8_t`，溢出后回到 0 触发"快速重试"
位置：`src/Esp8266BaseWiFi.h:89`、`src/Esp8266BaseWiFi.cpp:305-318`

- `_scheduleRetry()` 单调 `_retryCount++`。
- 一直连不上的设备过 ~256 次失败后会回到 0/1/2/3 区间，重新进入 fast retry，频繁敲门路由器。在长期连不上的弱信号场景下明显。

### B8（低）`Esp8266BaseConfig::_writeRaw` rename 序列在 LittleFS 上不是真正原子
位置：`src/Esp8266BaseConfig.cpp:137-155`

- 这是 LittleFS 的固有限制：每个 rename 各自落盘，但两次 rename 之间断电会留下 bak 而 path 不存在，或者 path 是新值而 bak 还在。
- `_readRaw` 的恢复逻辑能 handle 大多数情况，但综合 B4 / B5 一起看，断电恢复矩阵覆盖不全。

### B9（低）`Esp8266BaseLog::_writeFileBytes` 中存在条件不严的 continue
位置：`src/Esp8266BaseLog.cpp:362-369`

```cpp
if (_fileCurrentBytes >= _fileMaxBytes) {
    if (!_rotateFile() && !_truncateCurrentFile()) return false;
}
uint32_t room = _fileMaxBytes - _fileCurrentBytes;
size_t chunk = len - offset;
if (chunk > room) chunk = room;
if (chunk == 0) continue;
```

- `_writeFileBytes` 头部已经判 `!_fileMaxBytes` return false，正常情况下不会进入死循环。
- 但若 `_fileMaxBytes` 在调用过程中被改 0（运行时不太会发生）就有死循环风险。算"防御性编码缺陷"。

### B10（低）`_handleAuthPost` 写入新密码到 `eb_web_pass` 后没有 `flush()`
位置：`src/Esp8266BaseWeb.cpp:881-892`

- 这个 key 走的是 `setStr` 立即写，所以技术上不需要再 flush。
- 但如果用户改完密码立即拔电，由于 LittleFS 写入并不强制 fsync 到底层 NOR flash，重启后可能丢失。
- 改密成功后追加 `Esp8266BaseFileLog::flush()` 至少能保证文件日志内容也在断电前落盘。

### B11（低）`Esp8266BaseUtil::formatBytes` MB 公式在大值时溢出 uint32_t
位置：`src/Esp8266BaseUtil.h:20`

- `bytes * 10UL + 524288UL` 在 `bytes > ~409M` 时溢出。
- ESP8266 实际无法触达。`tools/check_logic.py` 的对照实现也只验证小值。
- 算无害实现细节，但若将来用在 Flash 容量、文件日志总大小等场景，要小心边界。

### B12（低）`Esp8266BaseWeb::_wb` 是全局 160B 共享缓冲，链式调用容易误用
位置：`src/Esp8266BaseWeb.cpp:164`

- 例如 `_handleLogsGet` 里 `_wb` 被 snprintf 多次，混合 segBuf 局部缓冲。
- 当前所有路径都"snprintf 写完 → 立即 sendChunk → 再 snprintf 覆盖"，没有真正交错，但只要未来在中间插入复杂逻辑就容易踩坑。
- 不是当前 bug，但是脆弱设计点。

### B13（低）`/health` 不要求 Auth，泄露 IP / heap / uptime / wifi 状态
位置：`src/Esp8266BaseWeb.cpp:1035-1047`

- 文档承认这是"健康检查"故意设计。
- 局域网内的扫描器可以无认证识别 ESP8266 设备并读到运行状态。如果设备暴露在更大网络上，这就是侦察面。

---

## C. 文档 / 代码漂移

### C1 `README.md` 给出的 `ESP8266BASE_WIFI_RETRY_FAST` 默认值与代码不一致
- `README.md:175` 写 `5000`
- 代码 `src/Esp8266BaseWiFi.h:30` 是 `2000`
- `CHANGELOG.md` 2026-05-07 明确说"从 5s 调整为 2s"
- `docs/01_overview.md` / `docs/03_api_reference.md` / `docs/08_networking.md` 都已是 `2000`
- **只有 README 漏改**

### C2 `docs/05_config_storage.md` 把 `eb_wdt_count` 写入方式标为 deferred
位置：`docs/05_config_storage.md:101`

- 实际 `src/Esp8266BaseWatchdog.cpp:66-68` 是 `setInt`（立即）+ `Esp8266BaseConfig::flush()`。

### C3 `docs/02_architecture.md` 关键不做项里仍然列着 "FileLog"
位置：`docs/02_architecture.md:235`

- 库已经实现 `Esp8266BaseFileLog` 并配套 `/logs` 页面、缓存机制、文档 `07_observability`。
- 这条直接和实现矛盾，是历史遗留文档。

### C4 `docs/03_api_reference.md` NTP 段落是合并 / 编辑残留
位置：`docs/03_api_reference.md:553-554`

```
... 对时成功后日志会输出当前实际时间...便于换算同步前的日志。
检查同步状态。系统 SNTP 或库内主动 UDP NTP 任一路径成功后...
```

- 一段写 `static bool begin()` 的描述里突然蹦出 `检查同步状态` 这句紧贴的句子，是 `static void handle()` 的解释错位粘到了 `begin()` 段后面。

### C5 `examples/wifi_config_ota/src/main.cpp` 注释写错认证
位置：`examples/wifi_config_ota/src/main.cpp:6`

- 头注释和 `curl -u admin:admin` 都写 `admin/admin`。
- 但该示例没有覆盖 `ESP8266BASE_WEB_AUTH_PASS`，实际是 `admin/esp8266`。

### C6 `docs/04_memory_budget.md` 的 RAM 预算与 `docs/02_architecture.md` 第九节有第二份副本，数值不一致
- 04 文件 Web 预算 `<= 880B`，02 文件 Web `<= 800B`
- 04 Log `<= 240B`，02 Log `<= 160B`
- 04 Config `<= 432B`，02 Config `<= 512B`
- 两份预算表数值都低于实际，但维持两份增加维护成本，且数据已经不同步。

### C7 `tools/test_all.sh` `--all-envs` 在 example 列表中故意排除 `full_demo`，文档没说明
- `tools/test_all.sh:41-44` 硬编码 `nodemcuv2` 只跑 4 个示例，因为 `examples/full_demo/platformio.ini` 没有 `nodemcuv2` env。
- 文档（`README.md`、`docs/11_maintainer_guide.md`）说"`tools/test_all.sh --all-envs` 会额外编译 `nodemcuv2` 环境"，没有提示 `full_demo` 不参与。
- 新增 example 时维护者必须改 `examples` 列表、`--all-envs` 列表、env 配置三处。

---

## D. 设计 / 健壮性脆弱点

### D1 `Esp8266BaseNTP::_sendManual` 在 `handle()` 路径上调用 `WiFi.hostByName()`
位置：`src/Esp8266BaseNTP.cpp:172`

- `hostByName` 是同步阻塞 DNS 查询，超时通常 2–5 秒。
- 当 DNS 全挂时，主 loop 单次 `handle()` 可能阻塞 5s 以上，远超 watchdog 默认 2.5s。
- 触发场景：上游路由器 DNS 暂时不可用、首次连接后 DNS 还没 ready。可能直接触发 watchdog 重启。

### D2 `Esp8266BaseWatchdog::handle()` 超时分支里仍然写 LittleFS
位置：`src/Esp8266BaseWatchdog.cpp:59-77`

```cpp
_resetCount++;
Esp8266BaseConfig::setInt(ESP8266BASE_CFG_KEY_WDT_COUNT,   (int)_resetCount);
Esp8266BaseConfig::setInt(ESP8266BASE_CFG_KEY_WDT_PENDING, 1);
Esp8266BaseConfig::flush();
ESP8266BASE_LOG_E(...);
Esp8266BaseFileLog::flush();
delay(50);
ESP.restart();
```

- 当主循环已经被卡住超过 timeout 时（往往是 LittleFS 或 WiFi 卡住），让看门狗去写 LittleFS 极易二次卡死。
- 总耗时几十毫秒；当前是一次性，不会反复重入，但风险面真实存在。

### D3 `Esp8266Base::begin()` 在 Config 初始化失败时仍继续运行
位置：`src/Esp8266Base.cpp:64-78`

- 后续 WiFi、Web、OTA 都在 `Config` 不可用的前提下启动。
- WiFi 读凭证拿默认空字符串 → 直接进 AP 配网；Web Auth 读 `eb_web_pass` 拿默认 → 默认密码；Watchdog 也读不到 wdt_count 等。
- 这些都"看起来工作"，但用户改的密码、Wi-Fi 凭证、WDT 历史在故障文件系统下静默丢失。
- 日志只一行 `littlefs_mount_failed config_disabled=yes`，缺少明显的运行警示（如启动时持续 LED 报警 / Web 首页横幅）。

### D4 `_loadAndIncrementBootCount` 在 Config 不可用时返回 0
位置：`src/Esp8266Base.cpp:3-30`

- 启动诊断会显示 `boot_count=0`，但其实是因为 Config 失败。
- 下游解析日志的人可能误以为是真正的第一次启动。

### D5 Web 模块路由表与 begin() 强耦合，`addPage/addApi` 必须在 begin 之后调用
- 如果业务先调 `addPage` 后调 `Esp8266Base::begin()`，`_running` 还是 false，页面路由也不会被注册。
- 代码不会在 `begin()` 里把已 addPage 的内容补注册。
- 文档反复强调"在 begin() 之后 addPage"，但缺乏运行时检查（业务调用顺序错时静默失败）。

### D6 `Esp8266BaseWeb::sendFooter()` 主动 `client().stop()`，但没有"sendFooter 后不要再写"的强约束
位置：`src/Esp8266BaseWeb.cpp:644-647`

- `_handleRebootPost`（line 1024-1027）在 `sendFooter()` 后又 `_server.client().stop()`，没有副作用。
- 但若业务 handler 在 `sendFooter` 后再 `sendChunk`，会写到已 stop 的 client，可能引起 SDK 警告或写失败。
- 文档没强调这一约束。

### D7 `Esp8266Base::handle()` 顺序里 NTP / mDNS 段没插入 watchdog feed
位置：`src/Esp8266Base.cpp:125-186`

- Web 段前后插入了 feed（line 174、178、185），但 NTP / mDNS 段没有。
- 如果 NTP `handle()` 阻塞（D1）或 mDNS `update()` 偶尔阻塞，watchdog 直到本轮末尾才检查，已经漏喂数百毫秒。

### D8 `Esp8266BaseSleep::deepSleep` 把 `Esp8266BaseWatchdog::pause()` 与 `Esp8266BaseConfig::flush()` 串行写 Flash
位置：`src/Esp8266BaseSleep.cpp:73-101`

- watchdog pause 后才 flush，若 flush 自身因 LittleFS 损坏卡住或 yield 进入未喂狗的循环，主循环不会触发库级 WDT 重启，只能依赖 SDK soft-WDT。
- 算"已知妥协"，但确实是一处脆弱点。

### D9 `Esp8266BaseWiFi::connect` POST 流程把凭证立即 `setStr` 到 Flash，再 `_startSTA`
位置：`src/Esp8266BaseWiFi.cpp:160-192`

- 即使新密码错了，旧凭证也已被覆盖。改密失败用户需要重新进入 AP 配网。
- 这是 `clearCredentials → 重启 → 再 AP` 的设计，但 Web 表单上不会提示"提交后无法回到旧凭证"。

---

## E. 测试 / 工具

### E1 `tools/check_logic.py` 不覆盖 README 默认值表
- 只验证 `WIFI_RETRY_FAST=2000`、`SLOW=60000`、`STUCK_DISCONNECTED_MS=7000`、`FAST_COUNT=3` 等少数项。
- 没有把 `README.md` 的"默认值表"也纳入校验，正是 C1 漂移没被自动捕获的原因。

### E2 `tools/check_static.sh` 不阻止默认凭证出现在示例的 release 编译
- 没有 grep `ESP8266BASE_WEB_AUTH_PASS=\\"esp8266\\"` 之类的校验。
- 生产 release 容易带默认密码出厂。

### E3 `tools/test_all.sh` 的 example 列表硬编码
位置：`tools/test_all.sh:27-44`

- 维护者新增 example 时必须改两处列表。
- 应改为遍历目录自动发现并按 env 自动匹配。

### E4 没有任何 host-side 单元测试或 mock 层
- `tools/check_logic.py` 仅复刻一两个公式，整个项目编译过即视为通过。
- `Config` / `Log` / WiFi 状态机这种容易出 corner case 的逻辑没有可重复运行的测试用例。

---

## F. 例程缺陷

### F1 `examples/sleep_watchdog` `freeze` 命令一旦触发后无法恢复
位置：`examples/sleep_watchdog/src/main.cpp:43-45`、`examples/sleep_watchdog/src/main.cpp:115-119`

- `freezeTest = true` 后没有清零，每次 loop 都会 `delay(5000)`。
- 第一轮就触发 watchdog 重启所以"看上去能工作"，但如果 watchdog 被禁用（`ESP8266BASE_USE_WATCHDOG=0`），设备就永远卡死。
- 默认开启 WDT，但作为教学代码注释里没说明这一隐式假设。

### F2 `examples/full_demo` deep sleep 表单等动作没有 CSRF 防护
- Basic Auth 浏览器会自动携带凭证，意味着已认证用户在恶意页面访问 `/api/ctrl?action=sleep` 就会触发设备睡眠。
- `g_sleepPending` 节流避免了重复触发，但其他动作如 `wdt_clear`、`cfg_write` 没有任何 CSRF 保护，恶意页面 + 同浏览器登录态可静默清掉 WDT 计数 / 写配置。
- 框架级别没有 CSRF token 机制；在文档"明确不支持"里也没列。

### F3 `examples/wifi_config_ota` 文档错误（参 C5）

---

## G. 其它小问题

### G1 `Esp8266BaseLog::log` 中 `tagBuf[5]` 用 `%-4.4s` 仅留 4 列宽
位置：`src/Esp8266BaseLog.cpp:470-471`

- 当 tag 是中文（多字节）时按字节截断会破坏字符。
- 代码里所有 tag 都是 ASCII，但维护规则没有显式禁止中文 tag。

### G2 `Esp8266BaseConfig::_setStrInternal` 重复读旧值（`_readRaw`）只为比较
位置：`src/Esp8266BaseConfig.cpp:244-253`

- 立即写每次都额外读一次 Flash + 解析。
- 对 `eb_boot_count` 这种每次启动都"值变化"的 key 无收益；可在 hot path 加一个"跳过比较"重载，但不算 bug。

### G3 `Esp8266BaseWeb::sendHeader` 调用 `_markRequest()` 后又在 `_handleRoot/_handleSystemHome` 等位置再次 `_markRequest()`
- 重复无害，但说明请求标记的入口分散，未来调试 `slow_request` 字段不容易跟踪到底以哪条记录为准。

### G4 `_handleSystemHome` 计算 boot time 的方式依赖 NTP 同步状态保持稳定
位置：`src/Esp8266BaseWeb.cpp:727`

- `time(nullptr) - (time_t)(millis()/1000UL)`
- 时区设置后 `time()` 返回带偏移的本地时间是没问题的（因为同步时是 UTC + offset 一并设的）。
- `NTP::reset` 会把 `_synced=false`，所以该路径在 `Esp8266BaseNTP::isSynced()` 真实反映状态时是安全的，但耦合脆弱。

---

## 综合判断

- **真正高优先级风险**：A1 / A2（Web 密码明文落盘）、B1（`configTime` 栈指针 UB）、B2（OTA watchdog pause/resume 单点恢复）、D1（DNS 阻塞触发 WDT）。这些都是明显违反"稳定优先 / 低风险"目标的点。
- **中等优先级**：B3 `clearAll` 迭代删除、B4 / B5 配置原子写边界、B6 / D3 / D4 Config 不可用降级行为、D2 watchdog 救场写 Flash 的死循环风险。
- **低优先级 / 文档维护**：C 系列文档漂移、E 系列测试覆盖不足、F2 框架缺 CSRF。

整体上库的设计与代码风格非常一致，绝大多数缺陷都是边界条件和文档同步问题，没有发现破坏 ESP8266-only 边界、违背 RAM 预算或导致正常路径功能失效的结构性缺陷。最值得在生产固件前优先修掉的是：A1 / A2、B1、B2、D1。
