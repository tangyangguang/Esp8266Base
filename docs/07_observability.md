# Esp8266Base 日志、审计与时间映射

> 版本：1.0.0  
> 模块：`Esp8266BaseLog` / `Esp8266BaseNTP` / `Esp8266BaseConfig`

---

## 一、组成

本库的观测能力包括：

- Serial 日志
- 可选 output hook
- 可选 LittleFS 文件日志 sink
- 启动会话日志
- 配置读写审计
- NTP 同步后的实际时间戳
- 对时前日志到实际时间的映射

默认只输出 Serial，不写文件，不启用配置审计。

---

## 二、日志等级

| 等级 | 名称 | 宏 |
|---:|---|---|
| 0 | DEBUG | `ESP8266BASE_LOG_D` |
| 1 | INFO | `ESP8266BASE_LOG_I` |
| 2 | WARN | `ESP8266BASE_LOG_W` |
| 3 | ERROR | `ESP8266BASE_LOG_E` |
| 4 | OFF | 关闭 |

低于编译期 `ESP8266BASE_LOG_LEVEL` 的宏会被编译掉。运行时可用 `Esp8266BaseLog::setLevel()` 调整 Serial 输出等级。

---

## 三、文件日志启用

示例：

```cpp
// 正式固件推荐：文件日志默认 WARN，不占用低优先级缓存 RAM。
Esp8266BaseLog::enableFileSink("/logs/app.log", 16 * 1024);

// 调试样例可把文件日志调到 INFO，记录更多上下文。
Esp8266BaseLog::enableFileSink("/logs/app.log", 16 * 1024, 1, 4);
```

参数：

| 参数 | 说明 |
|---|---|
| `path` | 当前日志文件路径，必须以 `/` 开头 |
| `maxBytes` | 单段最大字节数，最小 256 |
| `fileLevel` | 写入文件的最低等级，默认 `ESP8266BASE_LOG_FILE_LEVEL`，库默认 WARN |
| `rotateFiles` | 轮转段数，1-4，默认 4 |

即使 `fileLevel` 设置较高，WARN/ERROR 在 file sink 启用后也始终写入文件。

当编译期 `ESP8266BASE_LOG_FILE_BUFFER_SIZE>0` 且运行时 `fileLevel < WARN` 时，DEBUG/INFO 属于低优先级文件日志，会先进入小缓存，达到 `ESP8266BASE_LOG_FILE_FLUSH_INTERVAL_MS` 或缓存满后再写入 LittleFS。默认策略是：

| 场景 | 默认行为 |
|---|---|
| `ESP8266BASE_LOG_FILE_LEVEL >= WARN` | 不编译低优先级缓存，无 512B RAM 占用 |
| `ESP8266BASE_LOG_FILE_LEVEL < WARN` | 编译 512B 缓存，默认 2s 或满缓存刷盘 |
| WARN/ERROR | 始终立即写入文件，写入前先刷出已有低优先级缓存 |
| `/logs` 页面读取 | 读取文件前先调用 `flushFileSink()` |
| OTA 成功、Web 重启、deep sleep、看门狗重启 | 重启/休眠前调用 `flushFileSink()` |

---

## 四、4 段轮转逻辑

使用 4 段时文件为：

```text
/logs/app.log    current-0，当前正在写入
/logs/app.log.1  history-1，最近一次轮转出的历史
/logs/app.log.2  history-2
/logs/app.log.3  history-3，最旧历史
```

所有新日志都写入 `/logs/app.log`。当下一条日志会超过 `maxBytes` 时，先轮转，再写入新日志。

第 1 次写满：

```text
app.log -> app.log.1
app.log 重新创建为空，然后写新日志
```

第 3 次写满后通常为：

```text
app.log    当前最新
app.log.1  第 3 次前的当前文件
app.log.2  第 2 次前的当前文件
app.log.3  第 1 次前的当前文件
```

第 5 次及更多次写满：

```text
删除最旧 app.log.3
app.log.2 -> app.log.3
app.log.1 -> app.log.2
app.log   -> app.log.1
app.log 重新创建为空
```

因此最多保留最近约 `maxBytes * rotateFiles` 的日志，继续写入时会淘汰最旧段。

---

## 五、性能策略

文件日志为轻量实现：

- 目录只在首次写文件时准备一次。
- 当前文件大小用 `_fileCurrentBytes` 内存缓存。
- 每条日志不再反复 `open + size + close` 查询大小。
- 每条写入仍采用打开、追加、关闭，降低长时间打开文件带来的断电风险。

---

## 六、异常和断电策略

日志文件不是数据库。设计目标是：允许极端情况下丢少量日志，但不能让日志功能长期失效。

当前策略：

- 普通追加时断电：可能丢最后一条或出现最后一行不完整。
- 轮转中断电：可能少一段历史，或段号不连续。
- 轮转失败：截断当前 `/logs/app.log` 恢复写入。
- 追加打开失败：截断当前文件后重试一次。
- 追加写入字节数不匹配：重新读取当前文件大小，返回失败；Serial 日志不受影响。

这样做牺牲极端情况下的少量历史日志，换取 `/logs` 和 file sink 长期可用。

---

## 七、清除日志

`POST /logs/clear` 会清除：

```text
/logs/app.log
/logs/app.log.1
/logs/app.log.2
/logs/app.log.3
```

然后重新创建空的 `/logs/app.log`。清除后下一条日志继续写入 `/logs/app.log`。

清除操作保护：

- 需要 Basic Auth。
- 页面有 `confirm()` 二次确认。
- 表单使用 `once(this)` 防止连续点击重复提交。

手工重复 POST 仍会再次清空，但清空操作是幂等的。

---

## 八、配置审计

启用：

```cpp
Esp8266BaseLog::enableConfigAudit(true);
Esp8266BaseLog::enableConfigReadAudit(false);
```

写审计记录：

- key
- old/new
- changed/no_change
- immediate/deferred
- flush result

读审计默认建议关闭，避免页面/API 高频读取刷屏。配置审计不做敏感 key 脱敏，所有值直接输出；WiFi 密码明文日志是本库设计要求。

读审计的默认日志等级是 DEBUG，由 `ESP8266BASE_CFG_READ_AUDIT_LEVEL` 控制。普通 INFO 构建即使调用 `enableConfigReadAudit(true)`，也不会输出大量读审计日志；需要追踪每次配置读取时，把 `ESP8266BASE_LOG_LEVEL` 和 `ESP8266BASE_LOG_FILE_LEVEL` 调到 DEBUG，或单独提高 `ESP8266BASE_CFG_READ_AUDIT_LEVEL`。`getInt()` / `getBool()` 内部读取原始字符串时不会再额外输出 `getStr` 审计，避免一读两条日志。

---

## 九、启动会话日志

每次启动会写明显分割线：

```text
[925][I][Boot] ============================================================
[1055][I][Boot] boot_session_start boot_count=1
[1062][I][Boot] boot_reason=power-on boot_desc=上电或外部复位
[1070][I][Boot] firmware=full-demo version=1.0.0 free_heap=39.8 KB
[1188][I][Boot] ============================================================
```

`boot_reason` 是 ESP8266 SDK reset info 的归类结果，不输出 ESP32 风格的 `wake_reason`。当前映射：

| boot_reason | boot_desc |
|---|---|
| `power-on` | 上电或外部复位 |
| `deep-sleep` | 深度睡眠唤醒 |
| `soft-restart` | 软件重启 |
| `wdt-reset` | 看门狗重启 |
| `unknown` | 未知启动原因 |

无法识别的值统一输出 `unknown boot_desc=未知启动原因`，不会输出 `undefined`。

`eb_boot_count` 使用无符号十进制字符串保存，达到 `4,294,967,295` 后饱和。

---

## 十、NTP 时间映射

NTP 同步前日志时间戳是 `millis()`：

```text
[32780][I][NTP ] time_synchronized actual_time=2026-05-04 18:46:52 uptime_ms=32778 boot_time=2026-05-04 18:46:20
```

同步成功后输出映射：

```text
[32955][I][NTP ] time_mapping boot_millis=0 actual_time=2026-05-04 18:46:20 current_millis=32778 current_time=2026-05-04 18:46:52
```

然后日志时间戳切换为实际时间：

```text
[2026-05-04 18:46:52][I][NTP ] log_timestamp_mode=absolute_datetime
```

对时前日志可通过 `boot_time + millis` 换算为实际日期时间。

---

## 十一、Web 查看

访问：

```text
http://<device-ip>/logs
```

页面显示：

- file sink 是否启用
- 路径
- 轮转段数
- 文件等级，例如 `WARN (2)`
- 低优先级缓存状态
- 缓存已用/总大小和刷盘间隔（仅文件等级低于 WARN 且缓存编译启用时）
- 单段上限
- 总上限
- 每段大小
- 文件标签：`current-0`、`history-1`、`history-2`、`history-3`
- 当前选中日志段内容

`/logs` 默认显示最新的 `current-0`，不会一次输出所有轮转段，避免 4 段都有内容时页面过大。点击顶部文件标签会访问 `/logs?seg=1`、`/logs?seg=2` 等，只切换当前展示的单个文件。

---

## 十二、推荐配置

ESP8266 推荐：

```cpp
Esp8266BaseLog::enableFileSink("/logs/app.log", 16 * 1024);
```

4×16KB 通常足够定位现场问题，同时不会让 `/logs` 页面过慢。正式固件建议文件日志保持默认 WARN，磨损和页面读取成本都低；调试样例可使用 INFO，并启用 512B/2s 低优先级缓存减少 DEBUG/INFO 的小写入次数。日志特别多时可以调高单段大小，但会增加 LittleFS 占用和页面读取时间。
