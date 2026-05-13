# Esp8266Base 项目总览

> 版本：1.0.0  
> 平台：ESP8266 Arduino Core（仅限）  
> 前缀：主类 `Esp8266Base`，模块类 `Esp8266Base<模块名>`，宏 `ESP8266BASE_*`

---

## 一、项目定位

`Esp8266Base` 是专为 ESP8266 设计的轻量级基础库。核心取舍原则：

- RAM 余量 > 功能完整性
- 稳定运行 > 功能丰富
- 简单排错 > 复杂抽象
- 单平台专注 > 跨平台通用

代码中无任何 `#ifdef ESP32` 或跨平台分支，永远不添加。

---

## 二、目录结构

```text
Esp8266Base/
├── library.json                   # PlatformIO 库描述
├── README.md                      # 快速上手
├── docs/
│   ├── 00_user_guide.md           # 使用者主线指南
│   ├── 01_overview.md             # 本文件：项目总览
│   ├── 02_architecture.md         # 模块架构与依赖关系
│   ├── 03_api_reference.md        # 完整 API 参考
│   ├── 04_memory_budget.md        # RAM 预算与控制规则
│   ├── 05_config_storage.md       # 配置存储设计
│   ├── 06_web_ota.md              # Web 与 OTA
│   ├── 07_observability.md        # 日志、审计与时间映射
│   ├── 08_networking.md           # WiFi、NTP 与 mDNS
│   ├── 09_power_watchdog.md       # Sleep 与 Watchdog
│   ├── 10_troubleshooting.md      # 故障排查
│   └── 11_maintainer_guide.md     # 维护者指南
├── examples/
│   ├── basic_wifi/                # WiFi STA/AP 配网示例
│   ├── wifi_config_ota/           # Web 配网 + OTA 示例
│   ├── custom_web/                # 自定义 Web 页面示例
│   ├── sleep_watchdog/            # Sleep + Watchdog 示例
│   └── full_demo/                 # 全模块演示（参考实现）
├── src/
│   ├── Esp8266Base.h / .cpp       # 主入口
│   ├── Esp8266BaseLog.h / .cpp    # 日志
│   ├── Esp8266BaseConfig.h / .cpp # 配置存储
│   ├── Esp8266BaseWiFi.h / .cpp   # WiFi
│   ├── Esp8266BaseWeb.h / .cpp    # Web 管理控制台
│   ├── Esp8266BaseOTA.h / .cpp    # OTA 固件更新
│   ├── Esp8266BaseNTP.h / .cpp    # NTP 网络对时
│   ├── Esp8266BaseMDNS.h / .cpp   # mDNS hostname.local
│   ├── Esp8266BaseSleep.h / .cpp  # 深度睡眠 / Modem sleep
│   └── Esp8266BaseWatchdog.h / .cpp # 软件看门狗
└── partitions/
    └── esp8266-4mb-2mfs.ld        # 4MB Flash 自定义分区脚本
```

---

## 三、命名约定

| 类型 | 规则 | 示例 |
|------|------|------|
| 主入口类 | `Esp8266Base` | `Esp8266Base::begin()` |
| 模块类 | `Esp8266Base<Module>` | `Esp8266BaseLog`，`Esp8266BaseWiFi` |
| 编译宏 | `ESP8266BASE_*` | `ESP8266BASE_LOG_LEVEL` |
| 日志宏 | `ESP8266BASE_LOG_D/I/W/E` | `ESP8266BASE_LOG_I("WiFi", "connected")` |
| 源文件 | `Esp8266Base<Module>.h/.cpp` | `Esp8266BaseConfig.h` |

---

## 四、模块一览

| 模块 | 类名 | 主要职责 |
|------|------|----------|
| 主入口 | `Esp8266Base` | 初始化协调、统一 handle |
| 日志 | `Esp8266BaseLog` | 串口日志、编译期等级、时间戳 |
| 配置 | `Esp8266BaseConfig` | LittleFS KV 存储、deferred 写入 |
| WiFi | `Esp8266BaseWiFi` | STA 连接、AP 配网、状态机 |
| Web | `Esp8266BaseWeb` | 极简管理页、Basic Auth、内置改密、应用扩展 |
| OTA | `Esp8266BaseOTA` | Web OTA 上传、进度显示、WDT 联动 |
| NTP | `Esp8266BaseNTP` | 网络对时、日志时间切换 |
| mDNS | `Esp8266BaseMDNS` | hostname.local、_http._tcp 广播 |
| Sleep | `Esp8266BaseSleep` | modem/deep sleep 封装、唤醒原因 |
| Watchdog | `Esp8266BaseWatchdog` | 主循环活性监控、WDT 计数持久化 |

---

## 五、编译配置

`platformio.ini` 参考：

```ini
[platformio]
src_dir = examples/full_demo/src

[env:esp12f]
platform             = espressif8266
board                = esp12e
framework            = arduino
monitor_speed        = 115200
upload_speed         = 460800
lib_extra_dirs       = .
board_build.ldscript = partitions/esp8266-4mb-2mfs.ld

lib_deps =
    LittleFS

build_flags =
    -DESP8266BASE_LOG_LEVEL=1
    -DESP8266BASE_DEFAULT_HOSTNAME=\"esp8266base-full\"
    -DESP8266BASE_USE_WEB=1
    -DESP8266BASE_USE_OTA=1
    -DESP8266BASE_USE_NTP=1
    -DESP8266BASE_USE_MDNS=1
    -DESP8266BASE_USE_SLEEP=1
    -DESP8266BASE_USE_WATCHDOG=1
    -DESP8266BASE_WEB_MAX_APP_PAGES=4
    -DESP8266BASE_WEB_MAX_APP_APIS=6
    -DESP8266BASE_WEB_AUTH_USER=\"admin\"
    -DESP8266BASE_WEB_AUTH_PASS=\"admin\"
    -DESP8266BASE_NTP_TIMEZONE=28800
    -DESP8266BASE_WDT_TIMEOUT_MS=2500
```

编译宏说明：

| 宏 | 默认值 | 说明 |
|---|---|---|
| `ESP8266BASE_LOG_LEVEL` | `1` | 0=D, 1=I, 2=W, 3=E, 4=关闭 |
| `ESP8266BASE_DEFAULT_HOSTNAME` | `"esp8266base"` | 编译期默认 hostname，合法 `eb_hostname` 优先 |
| `ESP8266BASE_USE_WEB` | `1` | 编译 Web 管理页和 Web 扩展 API |
| `ESP8266BASE_USE_OTA` | `0` | 编译 OTA；要求 `ESP8266BASE_USE_WEB=1` |
| `ESP8266BASE_USE_NTP` | `0` | 编译 NTP 对时 |
| `ESP8266BASE_USE_MDNS` | `1` | 编译 mDNS |
| `ESP8266BASE_USE_SLEEP` | `1` | 编译 Sleep |
| `ESP8266BASE_USE_WATCHDOG` | `1` | 编译 Watchdog |
| `ESP8266BASE_WEB_MAX_APP_PAGES` | `4` | 应用页面最大数量 |
| `ESP8266BASE_WEB_MAX_APP_APIS` | `6` | 应用 API 最大数量 |
| `ESP8266BASE_WEB_AUTH_USER` | `"admin"` | Basic Auth 编译期默认用户名 |
| `ESP8266BASE_WEB_AUTH_PASS` | `"admin"` | Basic Auth 编译期默认密码 |
| `ESP8266BASE_NTP_TIMEZONE` | `28800` | 时区偏移秒（UTC+8） |
| `ESP8266BASE_WDT_TIMEOUT_MS` | `2500` | 看门狗超时毫秒 |
| `ESP8266BASE_CFG_DEFERRED_SIZE` | `4` | deferred 写入队列长度 |
| `ESP8266BASE_CFG_DEFERRED_FLUSH_INTERVAL_MS` | `5000` | deferred 写入最小刷盘间隔 ms |
| `ESP8266BASE_WIFI_CONNECT_TIMEOUT` | `20000` | WiFi STA 单次连接观察窗口 ms |
| `ESP8266BASE_WIFI_STA_SETTLE_MS` | `150` | STA 切换后调用 `WiFi.begin()` 前的稳定等待 ms |
| `ESP8266BASE_WIFI_RETRY_FAST` | `2000` | WiFi 快速重试间隔 ms |
| `ESP8266BASE_WIFI_RETRY_FAST_COUNT` | `3` | WiFi 快速重试次数 |
| `ESP8266BASE_WIFI_RETRY_SLOW` | `60000` | WiFi 慢速重试间隔 ms |

根目录配置使用 `full_demo` 作为默认构建入口；单独编译示例时进入 `examples/<name>` 目录运行 `pio run -e esp12f`。上传统一使用 `460800` baud。

必要自动化测试入口是 `tools/test_all.sh`。默认测试只做静态检查、轻量逻辑检查和 `esp12f` 编译矩阵，不烧录、不访问串口、不依赖硬件；`tools/test_all.sh --all-envs` 会额外编译根项目 `nodemcuv2` 和除 `full_demo` 外的示例 `nodemcuv2` 环境。

---

## 六、RAM 控制规则

以下规则不得违反：

1. 禁止全局大缓冲（> 512B）
2. 所有 HTML 内容放 `PROGMEM`，不保存在 DRAM
3. Web 页面使用 `sendContent_P()` / `sendChunk()` 流式输出，不拼接整页 `String`
4. 禁止 `std::function`，使用函数指针（`typedef void (*Handler)()`）
5. 禁止 STL 容器，使用固定大小静态数组
6. 禁止在模块全局状态中保存 `String` 对象，使用 `char[]`
7. 禁止递归
8. 每个新模块必须在 `docs/04_memory_budget.md` 中声明 RAM 预算

---

## 七、明确不支持

- ESP32 / ESP32-S3 / ESP32-C3
- HAL 抽象层
- 事件总线 / 通用 Scheduler
- 复杂 JSON API / 页面模板引擎
- 多用户权限 / HTTPS / WebSocket / 异步 Web
- POSIX 时区字符串 / 夏令时
- 动态路由表 / 无限制页面注册
