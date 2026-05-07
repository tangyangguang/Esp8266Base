# Esp8266Base 配置存储设计

> 版本：1.0.0  
> 模块：`Esp8266BaseConfig`  
> 文件：`Esp8266BaseConfig.h / .cpp`  
> 依赖：LittleFS

---

## 一、设计目标

- 使用 LittleFS 提供持久化 KV 存储
- 只支持必要类型：string / int32 / bool
- 写入前比较旧值，无变化不写 Flash
- 高频更新使用 deferred 机制延迟写入，分散 Flash 压力

---

## 二、文件系统布局

每个 key 对应 LittleFS 根目录下的一个文件：

```text
/
├── cfg_eb_wifi_ssid    → "IOTHOME"
├── cfg_eb_wifi_pass    → "secret123"
├── cfg_eb_wdt_count    → "5"
├── cfg_eb_wdt_pending  → "0"
└── cfg_<key>        → <value>
```

路径格式：`/cfg_<key>`，key 最大 24 字符，文件名最大 `5 + 24 = 29` 字符（LittleFS 限制 32 字符，安全）。

---

## 三、支持的数据类型

| 类型 | 存储格式 | 最大值长度 |
|------|---------|------------|
| string | UTF-8 文本 | 96 字节 |
| int32 | 十进制字符串，如 `"-12345"` | 11 字节 |
| bool | `"1"` 或 `"0"` | 1 字节 |

**不支持**：float、blob、namespace 枚举、数组。

---

## 四、立即写入 vs Deferred 写入

### 立即写入

`setStr / setInt / setBool` 立即写入 Flash。适用于：
- 低频配置（WiFi 凭证等）
- 重启前需要持久化的状态

调用流程：
```
setStr("key", "value")
  → 用栈上小缓冲读旧值比较
  → 相同：直接返回 true，不写 Flash
  → 不同：写入 /cfg_<key>.tmp → 读回校验
  → 旧文件改名为 .bak，tmp 提交为正式文件
  → 提交成功后删除 .bak → yield()
```

### Deferred 写入

`setIntDeferred / setBoolDeferred` 先入内存队列，由 `handle()` 分批写入。适用于：
- 高频更新的应用计数器
- 不需要立即持久化的运行状态

队列结构（静态，4 条）：

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

`handle()` 到达 `ESP8266BASE_CFG_DEFERRED_FLUSH_INTERVAL_MS` 间隔后最多写 1 条；同一 key 重复写入只保留最新值（覆盖队列中已有条目）。默认间隔为 5000ms，用于避免业务计数器每轮变化时持续刷写 Flash。

---

## 五、内置配置 Key 约定

以下 key 由库内部使用，统一使用 `eb_` 前缀，应用代码不得覆盖：

| Key | 类型 | 用途 | 写入方式 |
|-----|------|------|----------|
| `eb_wifi_ssid` | string | WiFi STA SSID | 立即 |
| `eb_wifi_pass` | string | WiFi STA 密码 | 立即 |
| `eb_ap_pass` | string | AP 配网密码 | 立即 |
| `eb_hostname` | string | 设备 hostname | 立即 |
| `eb_web_user` | string | Web Auth 持久化用户名，覆盖默认用户名 | 立即 |
| `eb_web_pass` | string | Web Auth 持久化密码，`/auth` 修改后写入，覆盖默认密码 | 立即 |
| `eb_wdt_count` | int32 | WDT 重启累计次数 | deferred |
| `eb_wdt_pending` | bool | 上次是否 WDT 重启 | 立即 |
| `eb_boot_count` | uint32 string | 启动次数，库自动维护，达到 4,294,967,295 后饱和 | immediate |

---

## 六、Flash 写入安全规则

**规则 1：写前比较**  
写入前先读取旧值，完全相同则跳过写 Flash，直接返回 `true`。

**规则 2：写后 yield**  
每次 Flash 写入完成后，立即调用 `yield()`，给 SDK 喂狗和处理 WiFi 事件留出时间。

**规则 3：分散写入和节流**
`handle()` 到达 deferred flush 间隔后最多处理 1 条 deferred，不在单次 handle 中批量写多条。业务每轮更新同一个 deferred key 时，只覆盖内存 pending 值，不会每轮写 Flash。

**规则 4：深睡/重启前 flush**  
进入 deep sleep 或调用 `ESP.restart()` 前，必须调用 `Esp8266BaseConfig::flush()` 写完所有 pending。

---

## 七、错误处理

| 错误情况 | 处理方式 |
|----------|----------|
| LittleFS 挂载失败 | 默认只重试并报错；`ESP8266BASE_CFG_FORMAT_ON_FAIL=1` 时才会自动 format 后重试 |
| key 超过 24 字符 | 返回 false，输出 WARN 日志 |
| string 值超过 96 字节 | 返回 false，输出 WARN 日志，不写 Flash |
| deferred 队列满 | 返回 false，输出 WARN 日志 |
| Flash 写入失败 | 返回 false，输出 ERROR 日志 |
| getStr 找不到 key | 写入默认值到 out，返回 false |

---

## 八、典型使用场景

### 应用自定义计数（deferred 写入）

```cpp
int32_t cnt = Esp8266BaseConfig::getInt("app_counter", 0) + 1;
Esp8266BaseConfig::setIntDeferred("app_counter", cnt);
// handle() 会在后续 loop() 中自动刷盘
```

`eb_boot_count` 是库保留 key，由 `Esp8266Base::begin()` 自动读取、递增并立即写入。内部用无符号十进制字符串保存，避免 `int32_t` 溢出；应用代码不要复用这个 key。

### WiFi 配网保存凭证（立即写入）

```cpp
Esp8266BaseConfig::setStr(ESP8266BASE_CFG_KEY_WIFI_SSID, ssid);
Esp8266BaseConfig::setStr(ESP8266BASE_CFG_KEY_WIFI_PASS, pass);
```

### Deep sleep 前强制刷盘

```cpp
Esp8266BaseConfig::flush();
Esp8266BaseSleep::deepSleep(60);
```

### 恢复出厂配置

```cpp
Esp8266BaseConfig::clearAll();
ESP.restart();
```

`clearAll()` 删除所有 `/cfg_*` 文件，包括 WiFi 凭证、Web 凭证、看门狗计数和应用自定义配置。适合长按按键恢复出厂配置。

---

## 九、RAM 预算

| 内容 | 大小 |
|------|------|
| deferred 队列（4条 × 34B） | 136B |
| _ready 标志 | 1B |
| audit 标志 | 2B |
| **小计** | **~139B + padding** |
