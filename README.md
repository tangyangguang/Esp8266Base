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

这些是硬件运行时目标，不能只用 PlatformIO 编译阶段的 RAM 用量替代；发布前仍以 `docs/04_memory_budget.md` 的规则和硬件验收为准。

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
    Esp8266BaseWeb::setDeviceName("Sensor Node");
    Esp8266BaseWeb::setHomePath("/sensor");
    Esp8266BaseWeb::setHomeMode(Esp8266BaseWebHomeMode::FUSED_HOME);
    Esp8266BaseWeb::setSystemNavMode(Esp8266BaseWebSystemNavMode::FOOTER_COMPACT);

    Esp8266Base::begin();

    // 注册自定义页面（begin() 之后调用）
    Esp8266BaseWeb::addPage("/sensor", "Sensor", handleSensorPage);
}

void loop() {
    Esp8266Base::handle();
}
```

首次使用：设备以 AP 模式启动，SSID `ESP8266-Config-XXXX`，连接后访问 `http://192.168.4.1/` 配置 WiFi 凭证。

业务项目可以把 `/` 配置为业务首页，基础库系统首页保留在 `/esp8266base`。未配置业务首页时，`/` 仍是 Esp8266Base 默认首页，保持开箱即用行为。

---

## 模块一览

| 模块 | 类名 | 主要职责 |
|------|------|----------|
| 主入口 | `Esp8266Base` | 初始化协调、统一 handle |
| 日志 | `Esp8266BaseLog` | 串口日志、编译期等级、时间戳 |
| 配置 | `Esp8266BaseConfig` | LittleFS KV 存储、deferred 写入 |
| WiFi | `Esp8266BaseWiFi` | STA 连接、AP 配网、状态机 |
| Web | `Esp8266BaseWeb` | 极简管理页、Basic Auth、内置改密、应用扩展 |
| OTA | `Esp8266BaseOTA` | Web OTA 上传、WDT 联动、业务安全生命周期回调 |
| MQTT | `Esp8266BaseMQTT` | 可选 TLS MQTT 传输、门控、退避、OTA 协调和诊断 |
| NTP | `Esp8266BaseNTP` | 网络对时、日志时间切换 |
| mDNS | `Esp8266BaseMDNS` | hostname.local、_http._tcp 广播 |
| Sleep | `Esp8266BaseSleep` | modem/deep sleep 封装、唤醒原因 |
| Watchdog | `Esp8266BaseWatchdog` | 主循环活性监控、WDT 计数持久化 |

---

## 目录结构

```
Esp8266Base/
├── README.md
├── CHANGELOG.md                    # 面向业务项目的能力变化记录
├── library.json
├── docs/
│   ├── 00_user_guide.md           # 使用者主线指南
│   ├── 01_overview.md              # 项目总览
│   ├── 02_architecture.md          # 架构与模块依赖
│   ├── 03_api_reference.md         # 完整 API 参考
│   ├── 04_memory_budget.md         # RAM 预算与控制规则
│   ├── 05_config_storage.md        # 配置存储设计
│   ├── 06_web_ota.md               # Web 与 OTA
│   ├── 07_observability.md         # 日志、审计与时间映射
│   ├── 08_networking.md            # WiFi、NTP 与 mDNS
│   ├── 09_power_watchdog.md        # Sleep 与 Watchdog
│   ├── 10_troubleshooting.md       # 故障排查
│   └── 11_maintainer_guide.md      # 维护者指南
├── src/
│   ├── Esp8266Base.h / .cpp        # 主入口
│   ├── Esp8266BaseLog.h / .cpp     # 日志
│   ├── Esp8266BaseConfig.h / .cpp  # 配置存储
│   ├── Esp8266BaseWiFi.h / .cpp    # WiFi
│   ├── Esp8266BaseWeb.h / .cpp     # Web 管理控制台
│   ├── Esp8266BaseOTA.h / .cpp     # OTA 固件更新
│   ├── Esp8266BaseMQTT.h / .cpp    # 可选 TLS MQTT 生命周期
│   ├── Esp8266BaseNTP.h / .cpp     # NTP 网络对时
│   ├── Esp8266BaseMDNS.h / .cpp    # mDNS hostname.local
│   ├── Esp8266BaseSleep.h / .cpp   # 深度睡眠 / Modem sleep
│   └── Esp8266BaseWatchdog.h / .cpp # 软件看门狗
├── examples/
│   ├── basic_wifi/                 # WiFi STA/AP 配网示例
│   ├── wifi_config_ota/            # Web 配网 + OTA 示例
│   ├── custom_web/                 # 自定义 Web 页面示例
│   ├── mqtt_terminal/               # MQTT_TERMINAL 通用连接和回调示例
│   ├── sleep_watchdog/             # Sleep + Watchdog 示例
│   └── full_demo/                  # 全模块演示（参考实现）
├── tools/
│   ├── test_all.sh                 # 必要自动化测试入口
│   ├── check_static.sh             # 静态一致性检查
│   └── check_logic.py              # 轻量逻辑检查
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
    -DESP8266BASE_DEFAULT_HOSTNAME=\"esp8266base-full\"
    -DESP8266BASE_USE_WEB=1
    -DESP8266BASE_USE_OTA=1
    -DESP8266BASE_USE_NTP=1
    -DESP8266BASE_USE_MDNS=1
    -DESP8266BASE_USE_SLEEP=1
    -DESP8266BASE_USE_WATCHDOG=1
    -DESP8266BASE_WEB_AUTH_USER=\"admin\"
    -DESP8266BASE_WEB_AUTH_PASS=\"admin\"
    -DESP8266BASE_WDT_TIMEOUT_MS=2500
    -DESP8266BASE_NTP_TIMEZONE=28800
```

| 宏 | 默认值 | 说明 |
|---|---|---|
| `ESP8266BASE_LOG_LEVEL` | `1` | 0=D 1=I 2=W 3=E 4=关闭 |
| `ESP8266BASE_DEFAULT_HOSTNAME` | `"esp8266base"` | 编译期默认 hostname，合法 `eb_hostname` 会在启动时覆盖 |
| `ESP8266BASE_FILELOG_DEFAULT_MODE` | `ESP8266BASE_FILELOG_MODE_ERROR` | 文件日志默认运行模式：OFF/ERROR/WARN/INFO |
| `ESP8266BASE_FILELOG_PATH` | `"/logs/app.log"` | 文件日志路径，构建期资源策略 |
| `ESP8266BASE_FILELOG_MAX_BYTES` | `16KB` | 文件日志单段最大字节数 |
| `ESP8266BASE_FILELOG_ROTATE_FILES` | `4` | 文件日志轮转段数，1-4 |
| `ESP8266BASE_FILELOG_BUFFER_SIZE` | `INFO 默认 512，否则 0` | INFO 文件日志低优先级缓存 |
| `ESP8266BASE_FILELOG_FLUSH_INTERVAL_MS` | `2000` | 低优先级文件日志缓存刷盘间隔 |
| `ESP8266BASE_CFG_READ_AUDIT_LEVEL` | `0` | 配置读审计等级，默认 DEBUG |
| `ESP8266BASE_USE_WEB` | `1` | 编译 Web 管理页 |
| `ESP8266BASE_PROFILE_MQTT_TERMINAL` | `0` | 正式 MQTT 智能终端模式；要求 Web/OTA/NTP/WDT/MQTT 全部启用 |
| `ESP8266BASE_USE_MQTT` | 跟随 `MQTT_TERMINAL` | 编译 `Esp8266BaseMQTT` 可选模块；要求 NTP |
| `ESP8266BASE_MQTT_RETRY_INITIAL_MS` | `2000` | MQTT 首次退避间隔 |
| `ESP8266BASE_MQTT_RETRY_MAX_MS` | `60000` | MQTT 指数退避上限 |
| `ESP8266BASE_USE_OTA` | `0` | 编译 OTA；要求 `ESP8266BASE_USE_WEB=1` |
| `ESP8266BASE_USE_NTP` | `0` | 编译 NTP 对时 |
| `ESP8266BASE_USE_MDNS` | `1` | 编译 mDNS |
| `ESP8266BASE_USE_SLEEP` | `1` | 编译 Sleep |
| `ESP8266BASE_USE_WATCHDOG` | `1` | 编译 Watchdog |
| `ESP8266BASE_WEB_MAX_APP_PAGES` | `4` | 应用页面上限 |
| `ESP8266BASE_WEB_MAX_APP_APIS` | `6` | 应用 API 上限 |
| `ESP8266BASE_WEB_AUTH_USER` | `"admin"` | Basic Auth 编译期默认用户名 |
| `ESP8266BASE_WEB_AUTH_PASS` | `"admin"` | Basic Auth 编译期默认密码 |
| `ESP8266BASE_CFG_FORMAT_ON_FAIL` | `0` | LittleFS 挂载失败时是否自动格式化；正式固件建议保持关闭 |
| `ESP8266BASE_WDT_TIMEOUT_MS` | `2500` | 看门狗超时 ms |
| `ESP8266BASE_NTP_TIMEZONE` | `28800` | 时区偏移秒（UTC+8） |
| `ESP8266BASE_NTP_SERVER_1..3` | 阿里云/腾讯云/cn.pool | NTP 服务器 |
| `ESP8266BASE_CFG_DEFERRED_FLUSH_INTERVAL_MS` | `5000` | deferred 写入最小刷盘间隔 ms；设为 0 可每轮最多刷 1 条 |
| `ESP8266BASE_WIFI_CONNECT_TIMEOUT` | `20000` | WiFi STA 单次连接观察窗口 ms |
| `ESP8266BASE_WIFI_STA_SETTLE_MS` | `150` | 切换 STA/断开旧状态后，调用 `WiFi.begin()` 前的稳定等待 ms |
| `ESP8266BASE_WIFI_RETRY_FAST` | `2000` | WiFi 快速重试间隔 ms |
| `ESP8266BASE_WIFI_RETRY_FAST_COUNT` | `3` | WiFi 快速重试次数，之后进入慢速重试 |
| `ESP8266BASE_WIFI_RETRY_SLOW` | `60000` | WiFi 慢速重试间隔 ms |

根目录 `platformio.ini` 使用 `examples/full_demo/src` 作为默认构建入口；各示例目录提供独立的 `platformio.ini`。上传建议使用 `460800` baud，避免部分 ESP8266 硬件在 `921600` 下出现 packet error。

WiFi 策略：没有保存凭证时进入 AP 配网；已有凭证但连接失败时，设备保持 STA 模式并按退避间隔持续重连，不自动打开配置 AP。需要重新进入 AP 配网时，先清除 WiFi 凭证再重启。

Web Auth 策略：认证用户名来自 `ESP8266BASE_WEB_AUTH_USER` 或 `Esp8266BaseWeb::setDefaultAuth()`；认证密码按 `ESP8266BASE_WEB_AUTH_PASS` → `setDefaultAuth()` → 设备已保存的 `eb_web_pass` 顺序确定，`setDefaultAuth()` 必须在 `Esp8266Base::begin()` 前调用。内置 `/auth` 页面可修改密码，保存后立即使用新密码，`clearAll()` 后恢复默认密码。

Hostname 策略：默认 hostname 来自 `ESP8266BASE_DEFAULT_HOSTNAME`；设备已保存的 `eb_hostname` 优先级最高。hostname 必须为 1-32 位小写字母、数字或短横线，不能以短横线开头或结尾，不允许 `.local`。System 页面和 `/api/system/hostname` 可保存新 hostname，重启后对 mDNS、Web 标题和设备发现生效；`clearAll()` 后恢复编译期默认值。

OTA 策略：`GET /ota` 页面和 `POST /ota` 上传都强制使用同一组 Basic Auth。上传页面使用 XMLHttpRequest 显示百分比、已上传大小和结果状态。

正式智能终端设置 `ESP8266BASE_PROFILE_MQTT_TERMINAL=1`。它保留 `GET /` 极小入口、WiFi/Auth、Health、`POST /ota` 与显式容量的应用路由；不编译完整首页、Logs、System、hostname、reboot、导航或 `GET /ota`。AP 配网时 `/` 返回 `303 /wifi`，STA 时返回 `303 /health`。应用页/API 容量设为 `0` 时不保留对应路由数组。

MQTT 连接配置必须在 `Esp8266Base::begin()` 前通过 `Esp8266BaseMQTT::configure()` 提供。host、clientId、用户名/密码和 LWT topic 会复制到固定缓冲；password 非空时 username 必须非空，LWT payload 非空时 willTopic 必须非空。`BearSSL::X509List` trust anchor 与可选 LWT payload 由业务长期持有并覆盖整个 MQTT 生命周期。配置可来自业务的私有构建配置或 `Esp8266BaseConfig`，库不新增 MQTT 持久化 key，也不接受不安全 TLS。不要把真实 broker 凭据写入仓库。

MQTT 使用 `bertmelis/espMqttClient 1.7.3` 同步 TLS 客户端。PlatformIO 构建需加入该依赖、`ESP32Async/ESPAsyncTCP 2.0.0`（上游 1.7.3 的显式 ESP8266 依赖）和 `lib_ldf_mode = deep+`，见 `examples/mqtt_terminal`。库不定义 `EMC_RX_BUFFER_SIZE`/`EMC_TX_BUFFER_SIZE`，并把 BearSSL 缓冲保持为 `4096/1024`。第三方会为 MQTT 出站队列、callback 包装、证书解析和 TLS 会话使用动态内存；基础库自身不调用 `new`/`malloc`。ESP8266 底层 DNS/TCP/TLS connect 单次尝试仍可能阻塞到网络超时，这是同步安全客户端的已知边界，外围状态机和退避不会忙循环。

连接回调之外还可注册 SUBACK、publish acknowledgement 和 client error 回调。SUBACK return code `0x80` 表示 broker 拒绝订阅；publish acknowledgement 对 QoS1 对应 PUBACK、对 QoS2 对应 PUBCOMP。TLS 失败会在 BearSSL transport 释放前保存 code/detail，`lastTlsErrorCode()`、`lastTlsErrorText()` 和断线日志可用于排障；`/health` 只输出错误码。每次新连接前会清除旧 TLS 错误。

OTA 仍只使用 `Esp8266BaseOTA`。业务 prepare callback 先检查执行器是否安全；通过后基础库自动停止 MQTT 消息分发并断开 MQTT/TLS，再调用 `Update.begin()`。任何失败恢复 Watchdog 与 MQTT 重连许可；成功保持 MQTT 关闭、flush 配置/日志并重启。业务不再需要自行释放基础 MQTT 模块，但仍负责执行器安全检查。

日志与回显策略：WiFi、Web Auth 和配置审计会有意输出明文值，并同时输出 `password_length` 等辅助字段；`/wifi` GET 表单也会回显已保存密码，页面默认隐藏，可手动显示。这是个人项目为了现场观察和调试保留的设计选择，不按缺陷处理；请只在可信串口/可信局域网环境中使用。

可选文件日志和配置审计：

```cpp
Esp8266BaseFileLog::setMode(Esp8266BaseFileLog::INFO);
Esp8266BaseLog::enableConfigAudit(true);
Esp8266BaseLog::enableConfigReadAudit(false);
```

文件日志运行时只支持 `OFF / ERROR / WARN / INFO` 四种模式，当前模式保存到 `eb_filelog_mode`；`DEBUG` 不作为文件日志模式。`ESP8266BASE_LOG_LEVEL` 仍是编译期上限，Web 和 public API 都不能突破。默认 ERROR 只在错误日志出现时写文件，默认长期运行的 Flash 磨损风险极低。`OFF` 不删除已有日志，清空内容仍由 System 页面中的 Clear logs 独立负责。path、单段大小、轮转段数、buffer 和 flush interval 都是构建期资源策略，不在 Web 普通运维界面暴露。完整逻辑见 `docs/07_observability.md`。

---

## 自动化测试

日常开发运行：

```bash
tools/test_all.sh
```

默认测试不烧录、不访问串口、不要求 ESP12F 在线。它执行格式、源码契约/顺序检查、纯退避策略检查，并编译根项目及全部示例的 `esp12f` 环境；`mqtt_terminal` 独立覆盖正式模式。它不动态验证 DNS/TLS、实际 MQTT 状态迁移、SUBACK/PUBACK 或 OTA。`--all-envs` 还编译根项目和可用示例的 `nodemcuv2` 环境：

```bash
tools/test_all.sh --all-envs
```

硬件烧录、WiFi 配网、OTA、deep sleep、GPIO 按钮等仍属于人工验收，不纳入默认自动化测试。

---

## 明确不支持

- ESP32 / ESP32-S3 / ESP32-C3
- HAL 抽象、事件总线、通用 Scheduler
- 复杂 JSON API、异步 Web（ESPAsyncWebServer）、HTTPS
- 基础库公共 API/自有状态中的 `std::function`、STL 容器（可选 `espMqttClient` 内部实现除外）
- 多用户权限、WebSocket

---

## 文档阅读顺序

使用者建议顺序：

1. `CHANGELOG.md` — 了解从 2026-05-06 起新增能力、行为变化和使用建议
2. `docs/00_user_guide.md` — 从零接入、配网、OTA、日志和 full_demo
3. `docs/07_observability.md` — 日志、审计、文件轮转和时间映射
4. `docs/10_troubleshooting.md` — 按现象排查问题

维护者建议顺序：

1. `docs/01_overview.md` — 项目定位
2. `docs/02_architecture.md` — 模块关系与初始化顺序
3. `docs/04_memory_budget.md` — RAM 约束，写代码前必读
4. `docs/11_maintainer_guide.md` — 维护规则与发布检查

查接口：`docs/03_api_reference.md`。专题细节：`docs/05_config_storage.md`、`docs/06_web_ota.md`、`docs/08_networking.md`、`docs/09_power_watchdog.md`。
