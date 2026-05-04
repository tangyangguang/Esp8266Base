# Esp8266Base 网络能力

> 版本：1.0.0  
> 模块：`Esp8266BaseWiFi` / `Esp8266BaseNTP` / `Esp8266BaseMDNS`

---

## 一、网络模块职责

- WiFi：STA 连接、AP 配网、保存/清除凭证、持续重连。
- NTP：联网后对时，输出实际时间和 boot time 映射。
- mDNS：广播 `hostname.local` 和 `_http._tcp` 服务。

NTP 和 mDNS 不在 `begin()` 中启动，而是在 `handle()` 中检测到 WiFi STA 已连接后触发。

---

## 二、WiFi 凭证

库保留 key：

| Key | 说明 |
|---|---|
| `eb_wifi_ssid` | STA SSID |
| `eb_wifi_pass` | STA 密码 |
| `eb_ap_pass` | 配网 AP 密码，空表示开放 AP |

WiFi 密码会在日志中明文输出，并带 `password_length`，用于现场观察和调试。

---

## 三、首次启动和 AP 配网

没有 `eb_wifi_ssid` 时设备进入 AP 配网：

```text
no_saved_wifi_credentials starting_config_ap ssid=ESP8266-Config-18E7
config_ap_started ssid=ESP8266-Config-18E7 ip=192.168.4.1 channel=6
```

默认 AP：

- SSID：`ESP8266-Config-XXXX`
- IP：`192.168.4.1`
- channel：6
- 密码：默认空，可通过 `eb_ap_pass` 配置

连接 AP 后访问 `http://192.168.4.1/wifi` 保存 STA 凭证。

---

## 四、有凭证时的连接策略

有 `eb_wifi_ssid` 时设备进入 STA 连接：

```text
station_connecting ssid=IOTHOME password=... password_length=11 keep_config_ap=no status=WL_DISCONNECTED status_code=6
```

如果连接失败，设备不会自动打开 AP，而是持续重连。这样可以避免上游 WiFi 临时不可用时设备错误进入配置 AP。

连接超时和重试会输出更完整的诊断字段：

```text
station_connect_timeout ssid=IOTHOME status=WL_NO_SSID_AVAIL status_code=1 elapsed=20000ms rssi=-76
station_reconnect_scheduled attempt=1 retry_in=5s mode=fast status=WL_DISCONNECTED status_code=6 rssi=-76
```

`status` 是 `WiFi.status()` 的可读名称，`status_code` 是原始数值。常见值包括 `WL_NO_SSID_AVAIL`、`WL_CONNECT_FAILED`、`WL_CONNECTION_LOST`、`WL_DISCONNECTED`。

重试策略：

| 阶段 | 间隔 |
|---|---|
| 前几次快速重试 | `ESP8266BASE_WIFI_RETRY_FAST`，默认 5s |
| 超过快速次数后 | `ESP8266BASE_WIFI_RETRY_SLOW`，默认 60s |

单次连接观察窗口为 `ESP8266BASE_WIFI_CONNECT_TIMEOUT`，默认 20s。

---

## 五、什么时候重新进入 AP

当前策略：已有凭证但连接不上时不自动进入 AP。

需要重新配网时：

- 调用 `Esp8266BaseWiFi::clearCredentials()` 后重启。
- 或调用 `Esp8266BaseConfig::clearAll()` 恢复出厂并重启。
- full_demo 中 GPIO0 长按 1 秒会清除全部配置并重启。

---

## 六、联网后的 Web 访问

STA 连接成功后日志示例：

```text
station_connected ip=192.168.2.114 gateway=192.168.2.1 dns=192.168.2.1 rssi=-70
```

可以通过：

```text
http://<ip>/
http://<hostname>.local/
```

访问 Web 管理页面。`hostname` 由 `Esp8266Base::setHostname()` 设置。

---

## 七、mDNS

mDNS 在 WiFi STA 连接后启动：

```text
mdns_started host=esp-demo.local service=http tcp_port=80
```

注意：

- mDNS 依赖局域网和客户端系统支持。
- 路由器隔离、手机热点、部分企业网络可能阻止 mDNS。
- mDNS 失败时仍可用 IP 访问。

---

## 八、NTP

NTP 在 WiFi STA 连接后启动：

```text
ntp_client_started timezone=UTC+8 servers=ntp.aliyun.com,ntp.tencent.com,cn.pool.ntp.org check_interval=5s manual_udp=yes
```

同步成功后输出：

```text
time_synchronized actual_time=2026-05-04 18:46:52 uptime_ms=32778 boot_time=2026-05-04 18:46:20
time_mapping boot_millis=0 actual_time=2026-05-04 18:46:20 current_millis=32778 current_time=2026-05-04 18:46:52
log_timestamp_mode=absolute_datetime
```

这用于把对时前的 `millis()` 日志换算为实际日期时间。

---

## 九、常见排查

| 现象 | 重点日志 |
|---|---|
| 进入 AP | `no_saved_wifi_credentials` |
| 有凭证但没连上 | `station_connect_timeout`、`station_reconnect_scheduled` |
| 密码错误 | 明文 password 日志、路由器认证日志 |
| mDNS 访问慢 | `mdns_started`、改用 IP 验证 |
| NTP 不同步 | `ntp_sync_pending`、DNS/网关/UDP 123 |

更多排查见 `docs/10_troubleshooting.md`。
