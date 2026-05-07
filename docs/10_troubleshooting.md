# Esp8266Base 故障排查

> 版本：1.0.0  
> 方式：按现象查日志，再定位模块

---

## 一、无法连接 WiFi，进入 AP

现象：

- 设备启动 AP：`ESP8266-Config-XXXX`
- 日志出现 `no_saved_wifi_credentials`

可能原因：

- 没有保存 `eb_wifi_ssid`。
- 恢复出厂或 `clearAll()` 删除了配置。
- 新版本改用 `eb_` 保留 key，不读取旧无前缀 key。

处理：

- 连接 AP，访问 `http://192.168.4.1/wifi` 重新配网。
- 确认 Web 表单 SSID/密码非空。

---

## 二、有凭证但持续重连

重点日志：

```text
loaded_saved_wifi_credentials ssid=... password=... password_length=...
station_connect_timeout ssid=... status=WL_NO_SSID_AVAIL status_code=1 elapsed=20000ms rssi=-76
station_reconnect_scheduled attempt=1 retry_in=2s mode=fast status=WL_DISCONNECTED status_code=6 rssi=-76
```

可能原因：

- 密码错误。
- 路由器信号弱或拒绝接入。
- 2.4GHz 网络不可用。
- DHCP 慢或路由器异常。

处理：

- 日志会明文显示密码，先核对密码。
- 看 `status`、`status_code`、RSSI、路由器 DHCP 列表。
- 临时靠近路由器测试。
- 清除凭证后重新配网。

---

## 三、mDNS 访问慢或失败

现象：

- `http://esp-demo.local/` 打不开。
- IP 可以访问。

重点日志：

```text
mdns_started host=esp-demo.local service=http tcp_port=80
```

可能原因：

- 客户端或网络不支持 mDNS。
- 路由器隔离 mDNS。
- WiFi 尚未连接。

处理：

- 先用 IP 访问确认 Web 正常。
- macOS/iOS 通常支持 mDNS；Windows 环境可能需要额外支持。
- 检查设备和客户端是否在同一局域网。

---

## 四、OTA Unauthorized 或上传失败

现象：

- OTA 上传返回 `Unauthorized`。
- 上传中断或失败。

处理：

- 重新打开 `/ota` 输入 Basic Auth。
- 确认 `ESP8266BASE_USE_WEB=1` 且 `ESP8266BASE_USE_OTA=1`。
- 确认固件适合当前分区空间。
- 上传时保持供电和 WiFi 稳定。

重点日志：

```text
ota_upload_route_registered method=POST path=/ota
```

---

## 五、NTP 不同步或没有实际时间

现象：

- 日志一直是 `[12345]` millis 时间戳。
- 没有 `time_synchronized`。

重点日志：

```text
ntp_client_started ...
ntp_sync_pending ...
time_synchronized actual_time=...
```

可能原因：

- WiFi 未连接。
- DNS/网关不可用。
- UDP 123 被阻断。
- NTP 服务器不可达。

处理：

- 先确认 `station_connected`。
- 查看 gateway/dns 日志。
- 换网络或 NTP server。

---

## 六、日志页为空或日志不轮转

现象：

- `/logs` 显示 disabled。
- 文件大小不增长。

处理：

- 确认调用了 `Esp8266BaseLog::enableFileSink()`。
- 确认 Config/LittleFS ready。
- 确认 fileLevel 没过滤 INFO；WARN/ERROR 会始终入文件。
- 查看 `/logs` 页面显示的 path、max per file、segments。

文件满时会轮转。极端异常时会截断当前文件恢复写入，允许丢少量日志但避免功能失效。

---

## 七、WDT 重启

重点日志：

```text
watchdog_timeout elapsed=... reset_count=... action=restart
boot_after_watchdog_reset reset_count=...
```

可能原因：

- `loop()` 阻塞。
- Web handler 做了耗时操作。
- 大量同步文件操作。
- 忘记调用 `Esp8266Base::handle()`。

处理：

- 缩短 handler 工作。
- 长操作拆分或 pause/resume Watchdog。
- 避免拼接大 String。

---

## 八、deep sleep 后无响应

这是正常现象。deep sleep 后 CPU 停止，Web 不会响应。

如果需要定时唤醒，ESP8266 必须 GPIO16 接 RST。没有这条线时，只能外部复位唤醒。

---

## 九、配置读写失败

重点日志：

```text
value too long
invalid key
deferred queue full
write failed
verify_failed
```

处理：

- key <= 24 字符。
- string value <= 96 字节。
- 高频写入使用 deferred，但队列只有 4 条。
- deep sleep/restart 前调用 `Esp8266BaseConfig::flush()`。

---

## 十、页面加载慢

重点日志：

```text
slow_request method=GET uri=/logs elapsed=...
```

可能原因：

- `/logs` 内容较多。
- WiFi RSSI 弱。
- 浏览器请求慢。
- handler 输出过大。

处理：

- 日志建议 4×16KB。
- 自定义页面使用流式输出，不拼整页 String。
- 优先用 IP 访问排除 mDNS 问题。
