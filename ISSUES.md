# Esp8266Base 问题记录

> 记录日期：2026-05-04  
> 来源：日志与配置模块磨损均衡分析

---

## P1 — 磨损均衡保护不足

### 问题 1：Config 高频计数器无寿命保护

**位置**：`Esp8266BaseConfig.cpp`

**现象**：高频变化的计数器（如 `eb_boot_count`、`eb_wdt_count` 或业务计数器）每次值变化都会触发 Flash 写入。即使使用 deferred 队列去重，只要值变了最终仍会写入。

**影响**：若设备每分钟重启一次，`eb_boot_count` 一年写入约 52 万次；若业务计数器每秒变化，年写入超 300 万次。ESP8266 Flash 典型寿命约 10 万次擦写，存在提前损坏风险。

**建议**：
- 对高频计数器增加"批量聚合"机制：deferred 队列中同 key 的多次变化只保留最终值（当前已实现覆盖，但 flush 时仍会写）
- 考虑为极高频 key 提供"内存-only"模式（不写 Flash，重启丢失）
- 或增加写入频率限制：同一 key 在 N 秒内最多写一次

---

### 问题 2：Log 文件持续追加无节流

**位置**：`Esp8266BaseLog.cpp:248` — `_writeFileLine()`

**现象**：每条日志都执行 `open("a")` → `print()` → `close()`。如果 Debug 级别开启且 `handle()` 中每轮 loop 都输出日志（10ms/轮），一天可写入 864 万条日志，远超 Flash 寿命。

**影响**：高频日志场景下，单文件 16KB 可能在几分钟内写满并触发轮转，轮转操作本身又产生大量元数据写入（remove + rename）。

**建议**：
- 增加日志节流：相同 tag+fmt 的日志在 N 秒内只写文件一次（Serial 不受影响）
- 或限制文件 sink 的写入频率：每秒最多写 N 条到文件
- 文档中明确警告高频日志的磨损风险

---

### 问题 3：每次 Config 写入产生 3-4 个文件操作

**位置**：`Esp8266BaseConfig.cpp:94-151` — `_writeRaw()`

**现象**：即使值变了只写一次数据，原子写入策略也会产生多次文件操作：
1. `LittleFS.remove(tmp)` — 清理旧临时文件
2. `LittleFS.open(tmp, "w")` + `f.print()` + `f.close()` — 写临时文件
3. `LittleFS.open(tmp, "r")` + `f.readBytes()` — 读回校验
4. `LittleFS.remove(bak)` — 清理旧备份
5. `LittleFS.rename(path, bak)` — 旧文件改备份
6. `LittleFS.rename(tmp, path)` — 临时文件提交
7. `LittleFS.remove(bak)` — 删除备份

**影响**：每次写入涉及 7 次文件系统操作，其中 5 次涉及元数据修改。LittleFS 虽然有底层磨损均衡，但持续写入同一 key 对应的文件会削弱其效果。

**建议**：
- 当前策略是数据安全优先（断电不丢数据），属于合理取舍
- 可在文档中说明此设计意图，让用户理解写入开销
- 对不要求断电安全的场景，可提供"快速写入"模式（直接覆盖，无 tmp/bak）

---

## P2 — 代码风格与一致性

### 问题 4：`clearFileSink()` 循环上限硬编码

**位置**：`Esp8266BaseLog.cpp:126`

```cpp
for (uint8_t i = 1; i < 4; i++) {  // 应使用 _fileRotateFiles
```

**现象**：`_fileRotateFiles` 可设为 1-4，但 `clearFileSink()` 始终循环到 4。当设为 2 或 3 时，仍会尝试删除不存在的 `.3` 文件（`LittleFS.exists()` 检查会跳过，但逻辑不一致）。

**建议**：使用 `for (uint8_t i = 1; i < _fileRotateFiles; i++)`

---

### 问题 5：`_handleWiFiPost()` 密码只修剪前导空白

**位置**：`Esp8266BaseWeb.cpp:503-504`

```cpp
_trimWhitespace(ssid);        // 修剪前后空白
_trimLeadingWhitespace(pass); // 只修剪前导空白
```

**现象**：SSID 修剪前后空白，但密码只修剪前导空白。如果用户输入 `"password  "`（尾部有空格），会被原样保存，可能导致连接失败。

**建议**：统一使用 `_trimWhitespace(pass)` 修剪前后空白。

---

### 问题 6：`AGENTS.md` 与 `CLAUDE.md` 内容重复

**位置**：根目录

**现象**：两个文件内容几乎完全相同（均为 154 行），修改一处容易遗漏另一处。

**建议**：保留一个作为主文件，另一个改为引用或 symlink；或明确两者的差异和维护策略。

---

### 问题 7：RAM 预算数据在多处文档重复

**位置**：`docs/02_architecture.md` 第九节 vs `docs/04_memory_budget.md` 第三节

**现象**：两处都有完整的 RAM 预算表，数据需要手动保持同步。

**建议**：`02_architecture.md` 中只保留摘要，链接到 `04_memory_budget.md` 获取完整数据。

---

## P3 — 设计脆弱点

### 问题 8：`_timeStr()` 静态缓冲非重入

**位置**：`Esp8266BaseNTP.cpp:246`

```cpp
const char* Esp8266BaseNTP::_timeStr() {
    static char buf[20];  // 非重入
```

**现象**：如果 log hook 中再次调用日志（嵌套），时间戳会被覆盖。当前单线程环境中不会发生，但设计脆弱。

**建议**：保持现状，文档已注明。如需改进，可在 `_timestamp()` 中直接复制字符串而非返回指针。

---

### 问题 9：`WiFi.hostByName()` 阻塞 DNS 查询

**位置**：`Esp8266BaseNTP.cpp:162`

**现象**：DNS 查询是阻塞调用，可能阻塞 `handle()` 1-3 秒（DNS 超时）。在此期间 WiFi 状态机、Web 请求、Watchdog 都不会推进。

**建议**：
- 可考虑使用异步 DNS 或缓存 IP 地址
- 或在文档中说明 NTP 初始化期间可能有短暂阻塞

---

### 问题 10：`begin()` 和 `setLevel()` 功能重复

**位置**：`Esp8266BaseLog.cpp:33-39`

```cpp
void Esp8266BaseLog::begin(uint8_t level) { _level = level; }
void Esp8266BaseLog::setLevel(uint8_t level) { _level = level; }
```

**现象**：两个方法功能完全相同，`begin()` 的默认参数 `ESP8266BASE_LOG_LEVEL` 与静态初始化值重复。

**建议**：保持现状（`begin()` 语义更清晰），或在 `begin()` 中增加其他初始化逻辑以区分。

---

## 问题汇总

| 编号 | 优先级 | 类别 | 模块 | 状态 |
|------|--------|------|------|------|
| 1 | P1 | 磨损均衡 | Config | 待修复 |
| 2 | P1 | 磨损均衡 | Log | 待修复 |
| 3 | P1 | 磨损均衡 | Config | 待评估（设计取舍） |
| 4 | P2 | 代码一致性 | Log | 待修复 |
| 5 | P2 | 代码一致性 | Web | 待修复 |
| 6 | P2 | 文档维护 | 根目录 | 待修复 |
| 7 | P2 | 文档维护 | docs/ | 待优化 |
| 8 | P3 | 设计脆弱点 | NTP | 可接受 |
| 9 | P3 | 设计脆弱点 | NTP | 待评估 |
| 10 | P3 | 代码冗余 | Log | 可接受 |
