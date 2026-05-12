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

`Esp8266BaseWiFi::connect()` 在保存前校验凭证长度：SSID 必须为 1-32 字节，密码必须为 0-63 字节。超限会拒绝保存并输出 `connect_rejected reason=ssid_too_long` 或 `reason=password_too_long`，避免 Config 保存值与实际连接缓存发生截断不一致。
密码可以为空，用于连接开放 WiFi；内置 `/wifi` 配网页也允许空密码。

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
station_connect_stuck_restarting ssid=IOTHOME status=WL_DISCONNECTED status_code=7 elapsed=7000ms restart_count=1 rssi=31
station_connect_stuck_retrying ssid=IOTHOME status=WL_DISCONNECTED status_code=7 elapsed=7000ms restart_count=1 rssi=-72
station_connect_timeout ssid=IOTHOME status=WL_NO_SSID_AVAIL status_code=1 elapsed=20000ms rssi=-76
station_reconnect_scheduled attempt=1 retry_in=2s mode=fast status=WL_DISCONNECTED status_code=6 rssi=-76
```

`status` 是 `WiFi.status()` 的可读名称，`status_code` 是原始数值。常见值包括 `WL_NO_SSID_AVAIL`、`WL_CONNECT_FAILED`、`WL_CONNECTION_LOST`、`WL_DISCONNECTED`。

重试策略：

| 阶段 | 间隔 |
|---|---|
| 前几次快速重试 | `ESP8266BASE_WIFI_RETRY_FAST`，默认 2s |
| 超过快速次数后 | `ESP8266BASE_WIFI_RETRY_SLOW`，默认 60s |

单次连接观察窗口为 `ESP8266BASE_WIFI_CONNECT_TIMEOUT`，默认 20s。每次切换到 STA 并断开旧状态后，会等待 `ESP8266BASE_WIFI_STA_SETTLE_MS`，默认 150ms，再调用 `WiFi.begin()`；这用于降低 ESP8266 上电后首轮连接停在 `WL_DISCONNECTED` 的概率。

如果连接开始后持续停在 `WL_DISCONNECTED` 且超过 `ESP8266BASE_WIFI_STUCK_DISCONNECTED_MS`，默认 7s，本库会记录 `station_connect_stuck_restarting` 并提前重启本轮 `WiFi.begin()`，避免 ESP8266 WiFi SDK 卡住时白等完整 20s。每个连接 attempt 最多做 1 次 stuck restart；如果重启后仍然卡住，会记录 `station_connect_stuck_retrying` 并进入快速重试，不再继续等满 20s。

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

库内主动 UDP NTP 只接受当前等待服务器的响应，并校验来源 IP、端口 123、server mode、stratum 和 leap indicator。无关 UDP 包会记录 `manual_ntp_packet_rejected` 并丢弃，不会改写系统时间。

---

## 九、常见排查

| 现象 | 重点日志 |
|---|---|
| 进入 AP | `no_saved_wifi_credentials` |
| 有凭证但没连上 | `station_connect_stuck_restarting`、`station_connect_stuck_retrying`、`station_connect_timeout`、`station_reconnect_scheduled` |
| 密码错误 | 明文 password 日志、路由器认证日志 |
| mDNS 访问慢 | `mdns_started`、改用 IP 验证 |
| NTP 不同步 | `ntp_sync_pending`、DNS/网关/UDP 123 |

更多排查见 `docs/10_troubleshooting.md`。
