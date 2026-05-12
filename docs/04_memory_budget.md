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
| Esp8266BaseLog | <= 240B 默认；INFO/DEBUG 文件缓存另加 <=512B | level/timeFn/hook/file sink path/current size；格式缓冲在栈上；默认文件等级 WARN 时不编译低优先级缓存 |
| Esp8266BaseConfig | <= 432B | deferred 队列 + _ready + audit flags + deferred flush timer |
| Esp8266BaseWiFi | <= 384B | 状态/计时器(18B) + _apSSID(28B) + _ip(16B) + _staSSID(64B) + _staPass(64B) |
| Esp8266BaseWeb（路由表） | <= 880B | AppRoute 4×48+6×48=480B + _authUser/Pass(48B) + _deviceName/_homePath(48B) + hostname/firmware/version/title/boot(116B) + labels(96B) + _activeUri/Method(37B) + 状态 |
| Esp8266BaseOTA | <= 160B | _inProgress/_rejected/_started/_status + 上传计时、字节数和 10% 进度日志状态 |
| Esp8266BaseNTP | <= 224B | 同步状态 + 检查计时器 + 主动 UDP NTP 状态 |
| Esp8266BaseMDNS | <= 96B | 运行状态 |
| Esp8266BaseSleep | <= 48B | _wakeReason ptr(4B) + _initialized(1B) + _modemSleeping(1B) |
| Esp8266BaseWatchdog | <= 96B DRAM + 12B RTC | timeout(4B) + 计时器(8B) + pause(1B) + count(4B)；RTC user memory word 64-66 保存 WDT 超时标记 |
| **库总计（自有）** | **< 2.5KB** | 不含 Arduino SDK 内部开销 |

库保留 RTC user memory：

| 地址 word | 字节数 | 用途 |
|---:|---:|---|
| 64-66 | 12B | Watchdog 超时标记，包含 magic、count、checksum |

业务代码如需使用 RTC user memory，必须避开库保留区域。

示例构建资源参考（PlatformIO `esp12f` release）：

下表是编译产物的 RAM/Flash 占用参考，用于观察趋势和回归；它不是运行时 free heap 实测。第二节的场景目标必须在硬件上结合 Web 页面、OTA、AP 配网等真实路径验收。

| 示例 | 启用模块 | RAM | Flash |
|------|----------|-----|-------|
| `basic_wifi` | Web/OTA/NTP/mDNS/Sleep/WDT 全关 | 33,212B | 313,251B |
| `sleep_watchdog` | Sleep + WDT | 33,028B | 314,419B |
| `custom_web` | Web + mDNS + WDT | 38,428B | 389,348B |
| `wifi_config_ota` | Web + OTA + NTP + mDNS + WDT | 41,568B | 414,028B |
| `full_demo` | Web + OTA + NTP + mDNS + Sleep + WDT | 43,580B | 422,616B |

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
static char _hostname[24];

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
- Web handler 中临时缓冲建议 <= 64B；较大缓冲使用全局共享 `_wb[160]`
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
