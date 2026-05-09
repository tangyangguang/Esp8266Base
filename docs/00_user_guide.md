# Esp8266Base 使用指南

> 版本：1.0.0  
> 平台：ESP8266 Arduino Core（仅限 ESP8266）  
> 读者：业务项目开发者

---

## 一、这个库解决什么问题

Esp8266Base 提供 ESP8266 项目常见基础能力：

- WiFi STA 连接与 AP 配网
- Web 管理页面与自定义页面/API
- Web OTA 固件升级
- 串口日志、文件日志、配置审计
- LittleFS 配置存储
- NTP 对时与 mDNS
- modem sleep / deep sleep 封装
- 软件 Watchdog 与重启计数

库的目标是轻量、可观察、可复用。它只支持 ESP8266，不包含 ESP32 兼容分支。

---

## 二、PlatformIO 配置

典型 `platformio.ini`：

```ini
[env:esp12f]
platform = espressif8266
board = esp12e
framework = arduino
monitor_speed = 115200
upload_speed = 460800
board_build.ldscript = partitions/esp8266-4mb-2mfs.ld
lib_deps = LittleFS

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
```

上传建议使用 `460800` baud。部分 ESP8266 硬件在 `921600` 下容易出现 packet error。

---

## 三、最小项目

```cpp
#include <Arduino.h>
#include "Esp8266Base.h"

void setup() {
    Serial.begin(115200);
    delay(100);

    Esp8266Base::setFirmwareInfo("my-device", "1.0.0");
    Esp8266Base::setHostname("esp-device");

    Esp8266Base::begin();
}

void loop() {
    Esp8266Base::handle();
}
```

`Esp8266Base::begin()` 初始化库模块。  
`Esp8266Base::handle()` 必须在 `loop()` 中持续调用，否则 WiFi、Web、NTP、mDNS、配置 deferred 写入和 Watchdog 都无法正常推进。

---

## 四、首次 WiFi 配网

没有保存 `eb_wifi_ssid` 时，设备会进入 AP 配网模式：

- AP SSID：`ESP8266-Config-XXXX`
- 默认 IP：`192.168.4.1`
- Web 认证：默认 `admin / esp8266`

连接 AP 后访问：

```text
http://192.168.4.1/
```

进入 WiFi 页面保存 SSID 和密码。保存后设备会尝试 STA 连接。WiFi 密码会在日志中明文输出，这是本库为了现场调试保留的设计。

如果已经保存凭证但连接失败，设备不会自动打开 AP，而是保持 STA 模式持续重连。需要重新配网时，清除 WiFi 凭证或恢复出厂配置后重启。

更多网络行为见 `docs/08_networking.md`。

---

## 五、Web 管理页面

启用 `ESP8266BASE_USE_WEB=1` 后，内置页面包括：

| 路由 | 说明 |
|---|---|
| `/` | 首页；可配置为业务首页 |
| `/esp8266base` | 基础库系统首页 |
| `/wifi` | WiFi 配置 |
| `/auth` | 修改 Web 密码 |
| `/ota` | OTA 上传 |
| `/logs` | 文件日志查看 |
| `/logs/clear` | 清空文件日志 |
| `/reboot` | 重启确认 |
| `/health` | 健康信息 JSON |

除 `/health` 外，内置管理页面使用 Basic Auth。默认认证来自 `ESP8266BASE_WEB_AUTH_USER/PASS`，业务代码可在 `Esp8266Base::begin()` 前调用 `Esp8266BaseWeb::setDefaultAuth(user, pass)` 覆盖默认值；设备已保存的 `eb_web_user` / `eb_web_pass` 优先级最高。Web 已启动后再调用 `setDefaultAuth()` 会被忽略。

访问 `/auth` 可修改 Web 密码。页面会校验当前密码、新密码和确认值，保存成功后写入 `eb_web_pass` 并立即使用新密码；`clearAll()` 后恢复默认认证。Web Auth 密码不会明文写入 Web 日志或 Config 审计日志。

表单页面使用轻量 JS 防重复提交，POST 后尽量重定向回 GET，避免刷新重复提交。

Web 与 OTA 细节见 `docs/06_web_ota.md`。

业务项目可以让业务页面成为主入口：

```cpp
Esp8266BaseWeb::setDeviceName("Sensor Node");
Esp8266BaseWeb::setHomePath("/sensor");
Esp8266BaseWeb::setHomeMode(Esp8266BaseWebHomeMode::FUSED_HOME);
Esp8266BaseWeb::setSystemNavMode(Esp8266BaseWebSystemNavMode::FOOTER_COMPACT);

Esp8266Base::begin();
Esp8266BaseWeb::addPage("/sensor", "Sensor", handleSensorPage);
```

`FUSED_HOME` 下 `/` 会 `303` 跳转到业务首页，`/esp8266base` 保留为基础库系统首页；`APP_HOME_FIRST` 下两者都跳转到业务首页。未配置时保持默认系统首页。

---

## 六、OTA 更新

启用 `ESP8266BASE_USE_OTA=1` 且 `ESP8266BASE_USE_WEB=1` 后，可访问：

```text
http://<device-ip>/ota
```

上传页面显示百分比、已上传大小和结果。OTA 上传期间 Watchdog 会暂停，结束后恢复。`GET /ota` 和 `POST /ota` 都使用同一组 Basic Auth。

---

## 七、启用文件日志和配置审计

默认只输出 Serial，不写文件。full_demo 使用：

```cpp
Esp8266BaseLog::enableFileSink("/logs/app.log", 16384);
Esp8266BaseLog::enableConfigAudit(true);
Esp8266BaseLog::enableConfigReadAudit(false);
```

文件日志默认 4 段轮转：

```text
/logs/app.log
/logs/app.log.1
/logs/app.log.2
/logs/app.log.3
```

极端情况下允许丢少量日志，但日志功能不能长期失效。轮转或追加打开异常时，库会截断当前文件恢复写入能力。

完整日志、审计和 NTP 时间映射说明见 `docs/07_observability.md`。

---

## 八、业务配置存储

使用 `Esp8266BaseConfig` 存储业务配置：

```cpp
Esp8266BaseConfig::setStr("my_name", "demo");
int32_t count = Esp8266BaseConfig::getInt("my_count", 0);
Esp8266BaseConfig::setIntDeferred("my_count", count + 1);
```

规则：

- key 最长 24 字符。
- string value 最长 96 字节。
- 库保留 key 使用 `eb_` 前缀，业务项目不要使用这个前缀。
- 高频计数使用 deferred 写入；默认每 5000ms 最多刷 1 条，同 key 高频更新只保留最新 pending 值。
- deep sleep 或重启前调用 `Esp8266BaseConfig::flush()`，并检查返回值；返回 `false` 表示至少一条 deferred 配置未写入成功。

配置存储细节见 `docs/05_config_storage.md`。

---

## 九、自定义 Web 页面和 API

自定义路由必须在 `Esp8266Base::begin()` 后注册：

```cpp
void handlePage() {
    if (!Esp8266BaseWeb::checkAuth()) return;
    Esp8266BaseWeb::sendHeader();
    Esp8266BaseWeb::sendContent_P(PSTR("<h2>Hello</h2>"));
    Esp8266BaseWeb::sendFooter();
}

void setup() {
    Serial.begin(115200);
    Esp8266Base::begin();
    Esp8266BaseWeb::addPage("/hello", "Hello", handlePage);
}
```

页面 HTML 放 PROGMEM，动态内容用小栈缓冲和 `sendChunk()` 分段输出，不拼接整页 `String`。

---

## 十、NTP、mDNS、Sleep、Watchdog

联网后，NTP 和 mDNS 由 `Esp8266Base::handle()` 自动触发：

- mDNS：`http://<hostname>.local/`
- NTP：对时成功后日志切换为实际时间，并输出 boot time 映射

Sleep 和 Watchdog 使用注意：

- modem sleep 只降低 WiFi modem 功耗，CPU 仍运行。
- deep sleep 后 CPU 停止，Web 不可访问；ESP8266 需要 GPIO16 接 RST 才能定时唤醒。
- Watchdog 监控主循环活性，OTA 期间会自动 pause/resume。

网络细节见 `docs/08_networking.md`。低功耗与 Watchdog 见 `docs/09_power_watchdog.md`。

---

## 十一、full_demo

`examples/full_demo/` 是完整参考实现，覆盖所有模块：

- 文件日志与配置审计
- WiFi 配网与 OTA
- 自定义 `/demo`、`/ctrl` 页面和 API
- NTP/mDNS
- Sleep/Watchdog
- GPIO0 长按 1 秒清除配置并重启
- GPIO2 板载 LED 状态指示

构建：

```bash
cd examples/full_demo
pio run -e esp12f
```

上传：

```bash
pio run -e esp12f -t upload --upload-port /dev/cu.usbserial-120
```

---

## 十二、下一步阅读

- API 查询：`docs/03_api_reference.md`
- Web/OTA：`docs/06_web_ota.md`
- 日志与审计：`docs/07_observability.md`
- 网络：`docs/08_networking.md`
- 低功耗与 Watchdog：`docs/09_power_watchdog.md`
- 故障排查：`docs/10_troubleshooting.md`
