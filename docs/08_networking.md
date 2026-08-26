# Esp8266Base 网络能力

> 版本：1.0.0  
> 模块：`Esp8266BaseWiFi` / `Esp8266BaseNTP` / `Esp8266BaseMDNS` / `Esp8266BaseMQTT`

---

## 一、网络模块职责

- WiFi：STA 连接、AP 配网、保存/清除凭证、持续重连。
- NTP：联网后对时，输出实际时间和 boot time 映射。
- mDNS：广播 `hostname.local` 和 `_http._tcp` 服务。
- MQTT：可选 TLS MQTT 传输、WiFi/NTP 门控、有界退避和连接诊断。

NTP 和 mDNS 不在 `begin()` 中启动，而是在 `handle()` 中检测到 WiFi STA 已连接后触发。

---

## 二、WiFi 凭证

库保留 key：

| Key | 说明 |
|---|---|
| `eb_wifi_ssid` | STA SSID |
| `eb_wifi_pass` | STA 密码 |

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
- 密码：空，开放 AP

连接 AP 后访问 `http://192.168.4.1/wifi` 保存 STA 凭证。

---

## 四、有凭证时的连接策略

有 `eb_wifi_ssid` 时设备进入 STA 连接：

```text
station_connecting ssid=IOTHOME password=... password_length=11 keep_config_ap=no status=WL_DISCONNECTED status_code=6
```

如果连接失败，设备不会自动打开 AP，而是持续重连。这是家庭设备的自恢复策略，不是连接失败兜底缺失：家庭场景更常见的是路由器重启、故障或上游 WiFi 暂时不可用，而不是用户修改了 SSID 或密码。保持 STA 重试后，路由器恢复时设备可以无需人工干预自动回连；如果超时切到 AP，设备会退出正常 STA 恢复路径，路由器恢复后反而不能自动回到原网络。

因此本库不采用“失败若干次后自动进入 AP”或常驻 `AP+STA`。自动 AP 会增加状态、无线暴露面和恢复歧义，也不符合当前个人家庭设备的最小策略。需要修改 SSID 或密码属于显式重新配网操作，应由用户清除凭证或恢复出厂触发，不能把暂时断网误判为配置变更。

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

当前策略：已有凭证但连接不上时不自动进入 AP，并持续保留原凭证等待路由器恢复。维护和业务项目评审不应把这一行为列为缺陷，除非对应产品已经明确改变网络恢复策略。

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

访问 Web 管理页面。hostname 由基础库启动期统一解析：合法持久化配置 `eb_hostname` 优先，其次使用编译期宏 `ESP8266BASE_DEFAULT_HOSTNAME`，兜底为 `esp8266base`。hostname 必须为 1-32 位小写字母、数字或短横线，不能以短横线开头或结尾，不允许 `.` 或 `.local`。System 页面和 `/api/system/hostname` 可以保存新的 `eb_hostname`，重启后对 mDNS、Web 标题和设备发现生效。

---

## 七、mDNS

mDNS 在 WiFi STA 连接后启动：

```text
mdns_started host=esp8266base-full.local service=http tcp_port=80
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

## 九、MQTT_TERMINAL 网络生命周期

`ESP8266BASE_PROFILE_MQTT_TERMINAL=1` 要求 Web、OTA、NTP、Watchdog 和 MQTT 同时启用。MQTT 只在 STA 已连接且 NTP 已同步后尝试 DNS/TCP/TLS/MQTT 建连；WiFi 或时间门控丢失时释放连接，恢复后重新进入退避状态机。

配置由业务在 `Esp8266Base::begin()` 前提供：host/clientId/username/password/LWT topic 复制到固定缓冲；trust anchor `BearSSL::X509List` 和可选 LWT payload 由业务持有且生命周期必须覆盖 MQTT。来源可以是业务私有构建配置或 Config，但仓库示例只含 `.invalid` host、占位 clientId 和公开根证书，不含真实凭据。基础库不新增 broker 配置持久化 key，也不在 `/health` 输出 host、clientId、用户名、密码或证书。

断线重试为 2s、4s、8s、16s、32s、60s，之后保持 60s。日志包含 `connect_attempt`、`connected`、`disconnected reason=`、`reconnect_scheduled` 以及 free heap/max block。TCP_DISCONNECTED 时若 BearSSL 存在错误，还会输出真实 `tls_code/tls_detail`；无 TLS 错误时不伪造。`lastTlsErrorCode()`/`lastTlsErrorText()` 提供只读诊断，`/health` 只含 code；新连接前清除旧值。

SUBACK return code `0x80` 会记录 `suback_rejected`，业务回调收到固定 `uint8_t` code 数组；publish acknowledgement 回调在 QoS1 PUBACK 或 QoS2 PUBCOMP 后触发。`OUT_OF_MEMORY`、`MAX_RETRIES`、`MALFORMED_PARAMETER` 等 client error 会映射为基础库枚举、记录日志并交给业务。自动源码检查只验证绑定、顺序、映射和退避策略，不等于真实 broker ACL、SUBACK/PUBACK 或 TLS 握手验证。

如果业务要求订阅确认后才算 ready，可在 SUBACK 不匹配、拒绝或其他应用握手失败时调用 `requestReconnect()`。请求在下一轮 `handle()` 执行断开并复用既有退避，不会在回调栈内同步断开或立即重连。

使用 `espMqttClient 1.7.3` 的同步安全客户端。MQTT 收发缓冲保持上游默认值，BearSSL 显式为 4096/1024；未降低证书校验。该客户端的大部分 MQTT 状态分步推进，但 ESP8266 底层 DNS/TCP/TLS connect 单次尝试可能阻塞到网络超时，无法宣称严格零阻塞；外围有界退避可避免忙循环。

---

## 十、常见排查

| 现象 | 重点日志 |
|---|---|
| 进入 AP | `no_saved_wifi_credentials` |
| 有凭证但没连上 | `station_connect_stuck_restarting`、`station_connect_stuck_retrying`、`station_connect_timeout`、`station_reconnect_scheduled` |
| 密码错误 | 明文 password 日志、路由器认证日志 |
| mDNS 访问慢 | `mdns_started`、改用 IP 验证 |
| NTP 不同步 | `ntp_sync_pending`、DNS/网关/UDP 123 |

更多排查见 `docs/10_troubleshooting.md`。
