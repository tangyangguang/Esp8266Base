# Esp8266Base RAM 预算与控制规则

> 版本：1.0.0
> 本文档是所有开发决策的资源约束基准。任何实现与 RAM 目标冲突时，以 **RAM 目标优先**。

---

## 一、ESP8266 内存概况

| 内存区域 | 总量 | 说明 |
|----------|------|------|
| DRAM | 80KB | 堆 + 静态数据 |
| 启动后可用堆（裸机） | ~50KB | 无任何业务 |
| WiFi 连接后可用堆 | ~35-40KB | WiFi SDK 消耗约 12-15KB |
| WebServer 启动后 | ~28-32KB | ESP8266WebServer 约 4-6KB |
| LittleFS 挂载后 | ~26-30KB | LittleFS 约 2-3KB |

**实际可用起点**：WiFi + WebServer + LittleFS 全开后，约 **28-30KB** free heap。

---

## 二、场景目标 free heap

| 场景 | 目标 free heap |
|------|----------------|
| 正常联网，Web 未活跃 | >= 24KB |
| Web 管理页面打开时 | >= 18KB |
| OTA 上传过程中最低值 | >= 12KB |
| AP 配网模式 | >= 18KB |

---

## 三、全局静态 RAM 预算表

模块通过 `ESP8266BASE_USE_*` 编译期开关裁剪。关闭模块时，对应 `.cpp` 会编译为空单元，模块的静态状态、页面字符串和大部分代码都不会进入最终固件；`ESP8266BASE_USE_OTA=1` 依赖 `ESP8266BASE_USE_WEB=1`，配置错误会在编译期报错。

| 模块 | 预算上限 | 主要来源 |
|------|---------|----------|
| Esp8266BaseLog | <= 16B | runtime/serial level + timeFn/hook/internal hook；格式缓冲在栈上 |
| Esp8266BaseFileLog | <= 80B 默认；INFO 文件缓存另加 <=512B | mode/path/current size/dir state；默认 ERROR 时不编译低优先级缓存 |
| Esp8266BaseConfig | <= 432B | deferred 队列 + _ready + audit flags + deferred flush timer |
| Esp8266BaseWiFi | <= 384B | 状态/计时器(18B) + _apSSID(28B) + _ip(16B) + _staSSID(64B) + _staPass(64B) |
| Esp8266BaseWeb | <= 1.20KB | ESP8266WebServer(~272B) + AppRoute 4×52+6×52=520B + auth/device/home/hostname/firmware/title/labels/active request + 4B 表单令牌；页面临时缓冲在栈上 |
| Esp8266BaseWeb（MQTT_TERMINAL 0+0 路由） | <= 640B | ESP8266WebServer + auth/device/hostname/firmware/active request + 4B 表单令牌；应用数组和完整导航状态均排除 |
| Esp8266BaseOTA | <= 160B | 上传状态/计时 + 64B 固定失败原因 + 三个可选生命周期函数指针 |
| Esp8266BaseMQTT | <= 3.0KB | `espMqttClientSecure`、固定配置/状态/回调及 96B TLS 错误文本；受控下线新增 12B 固定状态，不含动态 TLS/证书/LWT/final payload/出站队列 |
| Esp8266BaseNTP | <= 224B | 同步状态 + 检查计时器 + 主动 UDP NTP 状态 |
| Esp8266BaseMDNS | <= 96B | 运行状态 |
| Esp8266BaseSleep | <= 48B | _wakeReason ptr(4B) + _initialized(1B) + _modemSleeping(1B) |
| Esp8266BaseWatchdog | <= 96B DRAM + 12B RTC | timeout(4B) + 计时器(8B) + pause(1B) + count(4B)；RTC user memory word 64-66 保存 WDT 超时标记 |
| **核心裁剪目标（自有）** | **< 1.25KB** | Log + Config + WiFi + Sleep + Watchdog + 默认 FileLog，不含 Web/OTA/NTP/mDNS 和 Arduino SDK 内部开销 |
| **全模块默认目标（自有）** | **<= 2.9KB** | Web/OTA/NTP/mDNS/Sleep/Watchdog 全开，默认 ERROR FileLog，无 INFO 缓存 |
| **全模块 INFO FileLog 目标（自有）** | **<= 3.4KB** | 全模块默认目标 + INFO 文件日志缓存，缓存上限仍为单个静态缓冲 512B |

库保留 RTC user memory：

| 地址 word | 字节数 | 用途 |
|---:|---:|---|
| 64-66 | 12B | Watchdog 超时标记，包含 magic、count、checksum |

业务代码如需使用 RTC user memory，必须避开库保留区域。

示例构建资源参考（PlatformIO `esp12f` release）：

下表是编译产物的 RAM/Flash 占用参考，用于观察趋势和回归；它不是运行时 free heap 实测。第二节的场景目标必须在硬件上结合 Web 页面、OTA、AP 配网等真实路径验收。

| 示例 | 启用模块 | RAM | Flash |
|------|----------|-----|-------|
| `basic_wifi` | 基础裁剪，`ESP8266BASE_USE_MQTT=0`，无 MQTT 对象 | 33,972B | 315,051B |
| `sleep_watchdog` | Sleep + WDT | 33,884B | 316,259B |
| `custom_web` | Web + mDNS + WDT | 40,740B | 396,124B |
| `wifi_config_ota` | Web + OTA + NTP + mDNS + WDT | 44,276B | 421,784B |
| `full_demo` | 完整 Web + OTA + NTP + mDNS + Sleep + WDT，MQTT 排除 | 45,928B | 429,644B |
| `mqtt_terminal` | MQTT_TERMINAL 0+0 应用路由，含 MQTT 静态对象及受控下线示例；构建期/尚未建连 | 45,920B | 512,225B |

这些数值来自 PlatformIO 链接结果，只能证明静态 RAM/Flash 趋势。`MQTT_TERMINAL` 强制启用 MQTT，因此不存在“该模式但不含 MQTT 对象”的正式构建组合；无对象基线由 `basic_wifi` 给出。未连接、连接尝试、TLS 已连接和断开后的 free heap/max block 只能在真机测量，不能由 45,920B 静态 RAM 推算。TLS 诊断使用 96B 固定文本缓冲，不缩小通信缓冲。受控下线自身新增 `_shutdownActive`/result/packetId/deadline/timeout 共 12B；示例整体相对同工具链旧提交增加 544B RAM，差值还包含稳定结果名、精简诊断、LWT/final payload 与 OTA prepare 演示代码，不能全部归因于状态机。

正式 `MQTT_TERMINAL` 构建定义 `EMC_MIN_FREE_MEMORY=4096`。该值是 `espMqttClient 1.7.3` 创建出站包前检查的最大连续堆块门槛，不是静态预留；若真实分配失败，SUBSCRIBE/PUBLISH 仍返回 0。未定义 `EMC_RX_BUFFER_SIZE` 或 `EMC_TX_BUFFER_SIZE`，MQTT 收发保持上游默认值；BearSSL 显式保持 4096B RX / 1024B TX。第三方会为证书解析、TLS 会话、callback 包装与出站 MQTT packet 动态分配，峰值和碎片必须真机记录。

| MQTT_TERMINAL 真机场景 | Free heap | Max block | 状态 |
|---|---:|---:|---|
| 启动后、MQTT 尚未连接 | 待验证 | 待验证 | 本轮未烧录 |
| DNS/TCP/TLS 连接尝试 | 待验证 | 待验证 | 本轮未连接真实 broker |
| MQTT/TLS 已连接 | 待验证 | 待验证 | 本轮未连接真实 broker |
| 断开并释放 TLS 后 | 待验证 | 待验证 | 本轮未连接真实 broker |

`GET /health` 的 `heap` / `maxBlock` 是查询时的原始字节值，可用于记录 MQTT/TLS 已连接状态。OTA 验收还应记录 `ota_pause.heap/max`；该日志位于 TLS 释放判定后、`Update.begin()` 前，第二节的 OTA 目标以这个关键写入窗口为准。业务固件的静态 RAM 数值不能替代运行时指标，也不能直接与本表的示例构建数值比较。

Arduino SDK 内部开销（不可控，参考值）：

| 组件 | 估算 RAM |
|------|---------|
| WiFi SDK | ~12-15KB |
| ESP8266WebServer | ~4-6KB（含请求缓冲） |
| LittleFS | ~2-3KB |
| Arduino Core | ~3-4KB |

---

## 四、硬性 RAM 控制规则

**规则 1：禁止全局大缓冲**
不得声明 `static char buf[1024]` 或更大的全局/静态缓冲。单个临时缓冲默认不超过 512B。

**规则 2：禁止在常驻状态保存 HTML**
所有 HTML 内容必须放 `PROGMEM`，不得保存在 DRAM 字符串中。

```cpp
// 正确
static const char PAGE_HTML[] PROGMEM = "<html>...</html>";

// 错误
static String pageHtml = "<html>...</html>";
```

**规则 3：动态响应流式发送**
Web 页面必须使用 `sendContent_P()` / `sendChunk()` 流式输出，不得将整页 HTML 拼接为 `String` 后一次发送。

**规则 4：禁止 std::function**
每个 `std::function` 对象在 heap 上额外占用 16-24B。使用函数指针代替：

```cpp
// 正确
typedef void (*Esp8266BaseWebHandler)();

// 错误
std::function<void()> handler;
```

**规则 5：禁止 STL 容器**
不使用 `std::vector`、`std::map`、`std::list`。使用固定大小静态数组：

```cpp
// 正确
static AppRoute _pages[4];
static uint8_t _pageCount = 0;

// 错误
std::vector<AppRoute> _pages;
```

**规则 6：禁止长期保存 String 对象**
模块全局状态中不保存 `String`，使用 `char[]`：

```cpp
// 正确
static char _hostname[33];

// 错误
static String _hostname;
```

**规则 7：避免多模块重复保存配置**
`ssid` / `pass` 只在 WiFi 模块内存缓存一份，其他模块通过引用访问。

**规则 8：每模块必须有 RAM 预算**
新增模块必须在本文档第三节声明 RAM 预算。

---

## 五、栈使用注意

ESP8266 默认栈约 4KB：

- 日志格式化缓冲（128B）在栈上分配，不要在多层嵌套中重叠持有
- Web handler 中临时缓冲优先保持 <= 96B；JSON 响应等少数固定格式可使用 <= 160B 栈缓冲，但不要跨 helper 保存指针
- 禁止递归（快速消耗栈）

---

## 六、Flash 写入对实时性的影响

LittleFS 写入会阻塞 CPU 约 1-5ms：

- 不在 `loop()` 中每轮写 Flash
- 高频状态更新使用 `setIntDeferred` / `setBoolDeferred`
- 每轮 `handle()` 最多刷 1 条 pending，分散写入压力
- 写入前比较旧值，无变化不写（`setStr` 内置此逻辑）
- 每次写入后调用 `yield()`

---

## 七、RAM 监控

`GET /health` 返回实时 heap 信息（无需认证）：

```json
{"heap":43200,"maxBlock":43088,"ip":"192.168.1.100","uptime":123,"wifi":"connected"}
```

关注两个指标：
- `heap`：当前总空闲堆
- `maxBlock`：最大连续空闲块；低于 8KB 时说明碎片化严重

维护要求：新增模块或新增常驻状态时，必须同步本文件的预算表，并在 `docs/11_maintainer_guide.md` 的发布检查中确认构建后的 RAM 用量没有突破目标。
