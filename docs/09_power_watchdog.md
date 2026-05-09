# Esp8266Base Sleep 与 Watchdog

> 版本：1.0.0  
> 模块：`Esp8266BaseSleep` / `Esp8266BaseWatchdog`

---

## 一、能力范围

Sleep 模块负责：

- 识别启动/唤醒原因
- modem sleep
- deep sleep
- 进入睡眠前 flush 配置

Watchdog 模块负责：

- 监控主循环是否持续运行
- 超时时写入 RTC 标记并重启，避免在异常路径写 Flash
- 重启后识别上一次是否 Watchdog 重启
- 持久化 WDT reset count

---

## 二、modem sleep

modem sleep 由 ESP8266 SDK 管理 WiFi modem 低功耗。CPU 仍运行，`loop()` 和 Web 仍可工作。

典型日志：

```text
modem_sleep_enabled mode=sdk_managed_wifi_modem_sleep
```

modem sleep 适合空闲降低功耗，但不会像 deep sleep 那样关闭 CPU。

---

## 三、deep sleep

deep sleep 会停止 CPU，Web 页面不会继续响应。ESP8266 定时唤醒需要 GPIO16 接 RST。

进入 deep sleep 前库会 flush 配置：

```cpp
Esp8266BaseConfig::flush();
Esp8266BaseSleep::deepSleep(10);
```

典型日志：

```text
deep_sleep_requested source=web duration=10s wake_requires=GPIO16_to_RST
entering_deep_sleep duration=10s free_heap=38.0 KB
```

如果没有 GPIO16→RST，设备进入 deep sleep 后不会按定时自动回来，需要外部复位。

---

## 四、Watchdog 行为

Watchdog 在 `Esp8266Base::handle()` 末尾检查并喂狗。业务代码必须避免长时间阻塞，或者在可控的长操作前 pause/resume。

默认 timeout：

```ini
-DESP8266BASE_WDT_TIMEOUT_MS=2500
```

取值会 clamp 到 1000-3000ms。

---

## 五、WDT 持久化 key

| Key | 说明 |
|---|---|
| `eb_wdt_count` | WDT 重启累计次数，重启后的正常启动阶段补写 |
| `eb_wdt_pending` | 旧固件兼容标记；新超时路径不再依赖它 |

超时时只写 RTC user memory 标记，不写 LittleFS：

```text
watchdog_timeout elapsed=4200ms reset_count=4 action=restart
```

重启后在 Watchdog 初始化阶段补写 `eb_wdt_count`。只有 `eb_wdt_count` 和 `eb_wdt_pending` 都写入成功后，才清除 RTC 标记；如果 Flash 暂时不可写，下一次启动会继续尝试补写。

```text
boot_after_watchdog_reset reset_count=4 source=rtc persist=success
```

如果旧固件留下 `eb_wdt_pending`，仍会按兼容路径识别或清除 stale pending。

库保留 RTC user memory 区域：

| 地址 word | 字节数 | 用途 |
|---:|---:|---|
| 64-66 | 12B | Watchdog 超时标记：magic、count、checksum |

业务代码如果直接调用 `system_rtc_mem_read/write`，不得复用上述区域。

---

## 六、pause / resume

OTA 上传期间库会自动 pause/resume Watchdog。业务项目如有明确长阻塞操作，也可以：

```cpp
Esp8266BaseWatchdog::pause();
// long operation
Esp8266BaseWatchdog::resume();
```

`resume()` 会重置计时，避免暂停期间累计时间导致误触发。`ESP8266BASE_USE_WATCHDOG=0` 时，OTA 和 deep sleep 路径会跳过 Watchdog pause/resume。

---

## 七、full_demo 参考

full_demo 展示：

- GPIO0 长按 1 秒清除全部 `/cfg_*` 配置并重启。
- GPIO2 板载 LED 低电平亮。
- 联网常亮、AP 慢闪、连接中快闪。
- Web 触发 deep sleep 前有确认提示。

GPIO0 是 ESP8266 下载模式相关引脚，串口工具 RTS/DTR 或外部电路可能误拉低。调试时如看到连续 `button_long_press_detected`，先检查串口复位线和按键电路。

---

## 八、排查点

| 现象 | 检查 |
|---|---|
| deep sleep 后无响应 | 是否 GPIO16 接 RST，是否需要外部复位 |
| 频繁 WDT 重启 | 是否 loop 阻塞，Web handler 是否太慢，是否漏调用 `Esp8266Base::handle()` |
| WDT count 不变 | Config 是否 ready，`eb_wdt_count` 是否被清除 |
| OTA 期间 WDT | OTA pause/resume 日志，供电和 WiFi 稳定性 |

更多排查见 `docs/10_troubleshooting.md`。
