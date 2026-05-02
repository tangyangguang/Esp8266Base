# Esp8266Base

**版本：1.0.0**  
ESP8266 专用轻量基础库。RAM 优先设计，仅支持 ESP8266 Arduino Core。

> **平台**：ESP8266 only — 代码中无任何 ESP32 分支  
> **前缀**：主类 `Esp8266Base`，模块类 `Esp8266Base<Module>`，宏 `ESP8266BASE_*`

---

## 核心目标

| 场景 | 目标 free heap |
|------|----------------|
| 正常联网，Web 未活跃 | >= 24KB |
| Web 管理页面打开 | >= 18KB |
| OTA 上传过程中 | >= 12KB |
| AP 配网模式 | >= 18KB |

---

## 快速开始

```cpp
#include "Esp8266Base.h"

void handleSensorPage() {
    if (!Esp8266BaseWeb::checkAuth()) return;
    Esp8266BaseWeb::sendHeader();
    Esp8266BaseWeb::sendContent_P(PSTR("<h2>Sensor</h2><p>Temperature: 25.3 C</p>"));
    Esp8266BaseWeb::sendFooter();
}

void setup() {
    Serial.begin(115200);

    Esp8266Base::setFirmwareInfo("my-device", "1.0.0");
    Esp8266Base::setHostname("esp-device");  // 访问 http://esp-device.local/

    Esp8266Base::begin();

    // 注册自定义页面（begin() 之后调用）
    Esp8266BaseWeb::addPage("/sensor", handleSensorPage);
}

void loop() {
    Esp8266Base::handle();
}
```

首次使用：设备以 AP 模式启动，SSID `ESP8266-Config-XXXX`，连接后访问 `http://192.168.4.1/` 配置 WiFi 凭证。

---

## 模块一览

| 模块 | 类名 | 主要职责 |
|------|------|----------|
| 主入口 | `Esp8266Base` | 初始化协调、统一 handle |
| 日志 | `Esp8266BaseLog` | 串口日志、编译期等级、时间戳 |
| 配置 | `Esp8266BaseConfig` | LittleFS KV 存储、deferred 写入 |
| WiFi | `Esp8266BaseWiFi` | STA 连接、AP 配网、状态机 |
| Web | `Esp8266BaseWeb` | 极简管理页、Basic Auth、应用扩展 |
| OTA | `Esp8266BaseOTA` | Web OTA 上传、进度显示、WDT 联动 |
| NTP | `Esp8266BaseNTP` | 网络对时、日志时间切换 |
| mDNS | `Esp8266BaseMDNS` | hostname.local、_http._tcp 广播 |
| Sleep | `Esp8266BaseSleep` | modem/deep sleep 封装、唤醒原因 |
| Watchdog | `Esp8266BaseWatchdog` | 主循环活性监控、WDT 计数持久化 |

---

## 目录结构

```
Esp8266Base/
├── README.md
├── library.json
├── docs/
│   ├── 01_overview.md              # 项目总览
│   ├── 02_architecture.md          # 架构与模块依赖
│   ├── 03_api_reference.md         # 完整 API 参考
│   ├── 04_memory_budget.md         # RAM 预算与控制规则
│   ├── 05_config_storage.md        # 配置存储设计
│   └── 06_web_extension.md         # Web 扩展开发指南
├── src/
│   ├── Esp8266Base.h / .cpp        # 主入口
│   ├── Esp8266BaseLog.h / .cpp     # 日志
│   ├── Esp8266BaseConfig.h / .cpp  # 配置存储
│   ├── Esp8266BaseWiFi.h / .cpp    # WiFi
│   ├── Esp8266BaseWeb.h / .cpp     # Web 管理控制台
│   ├── Esp8266BaseOTA.h / .cpp     # OTA 固件更新
│   ├── Esp8266BaseNTP.h / .cpp     # NTP 网络对时
│   ├── Esp8266BaseMDNS.h / .cpp    # mDNS hostname.local
│   ├── Esp8266BaseSleep.h / .cpp   # 深度睡眠 / Modem sleep
│   └── Esp8266BaseWatchdog.h / .cpp # 软件看门狗
├── examples/
│   ├── basic_wifi/                 # WiFi STA/AP 配网示例
│   ├── wifi_config_ota/            # Web 配网 + OTA 示例
│   ├── custom_web/                 # 自定义 Web 页面示例
│   └── full_demo/                  # 全模块演示（参考实现）
└── partitions/
    └── esp8266-4mb-2mfs.ld         # 4MB Flash 分区脚本（2MB固件+2MB LittleFS）
```

---

## 编译配置

```ini
[env:esp12f]
platform             = espressif8266
board                = esp12e
framework            = arduino
monitor_speed        = 115200
upload_speed         = 460800
board_build.ldscript = partitions/esp8266-4mb-2mfs.ld
lib_deps             = LittleFS

build_flags =
    -DESP8266BASE_LOG_LEVEL=1
    -DESP8266BASE_USE_WEB=1
    -DESP8266BASE_USE_OTA=1
    -DESP8266BASE_USE_NTP=1
    -DESP8266BASE_USE_MDNS=1
    -DESP8266BASE_USE_SLEEP=1
    -DESP8266BASE_USE_WATCHDOG=1
    -DESP8266BASE_WEB_AUTH_USER=\"admin\"
    -DESP8266BASE_WEB_AUTH_PASS=\"esp8266\"
    -DESP8266BASE_WDT_TIMEOUT_MS=2500
    -DESP8266BASE_NTP_TIMEZONE=28800
```

| 宏 | 默认值 | 说明 |
|---|---|---|
| `ESP8266BASE_LOG_LEVEL` | `1` | 0=D 1=I 2=W 3=E 4=关闭 |
| `ESP8266BASE_USE_WEB` | `1` | 编译 Web 管理页 |
| `ESP8266BASE_USE_OTA` | `0` | 编译 OTA；要求 `USE_WEB=1` |
| `ESP8266BASE_USE_NTP` | `0` | 编译 NTP 对时 |
| `ESP8266BASE_USE_MDNS` | `1` | 编译 mDNS |
| `ESP8266BASE_USE_SLEEP` | `1` | 编译 Sleep |
| `ESP8266BASE_USE_WATCHDOG` | `1` | 编译 Watchdog |
| `ESP8266BASE_WEB_MAX_APP_PAGES` | `4` | 应用页面上限 |
| `ESP8266BASE_WEB_MAX_APP_APIS` | `6` | 应用 API 上限 |
| `ESP8266BASE_WDT_TIMEOUT_MS` | `2500` | 看门狗超时 ms |
| `ESP8266BASE_NTP_TIMEZONE` | `28800` | 时区偏移秒（UTC+8） |
| `ESP8266BASE_WIFI_RETRY_FAST_COUNT` | `3` | WiFi 快速重试次数，之后进入慢速重试 |

根目录 `platformio.ini` 使用 `examples/full_demo/src` 作为烟测入口；各示例目录仍保留自己的 `platformio.ini`。上传建议使用 `460800` baud，避免部分 ESP8266 硬件在 `921600` 下出现 packet error。

---

## 明确不支持

- ESP32 / ESP32-S3 / ESP32-C3
- HAL 抽象、事件总线、通用 Scheduler
- 文件日志（FileLog）
- 复杂 JSON API、异步 Web（ESPAsyncWebServer）、HTTPS
- `std::function`、STL 容器
- 多用户权限、WebSocket

---

## 文档阅读顺序

1. `docs/01_overview.md` — 整体定位，先读此文
2. `docs/02_architecture.md` — 模块关系与初始化顺序
3. `docs/03_api_reference.md` — 开发时查阅
4. `docs/04_memory_budget.md` — RAM 约束，写代码前必读
5. `docs/05_config_storage.md` — Config 模块细节
6. `docs/06_web_extension.md` — 添加自定义页面时查阅
