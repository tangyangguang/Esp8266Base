# Esp8266Base Sleep 与恢复监护

> 版本：1.0.0  
> 模块：`Esp8266BaseSleep` / `Esp8266BaseWatchdog`

## 一、职责

Sleep 负责识别启动/唤醒原因、modem sleep、deep sleep 和睡眠前配置下刷。

Watchdog 负责两类监护：

1. 主循环返回后的整轮超时诊断；
2. 独立 SDK `os_timer` heartbeat 监护，用于发现调用持续 `yield()` 但永不返回的冻结。

所有恢复重启先执行安全回调、写 RTC 原因和阶段，并受统一预算限制。

## 二、Sleep

modem sleep 由 SDK 管理，CPU 和 `loop()` 仍运行。deep sleep 会停止 CPU，定时唤醒要求 GPIO16 连接 RST。进入 deep sleep 前库会 flush 配置：

```cpp
Esp8266BaseConfig::flush();
Esp8266BaseSleep::deepSleep(10);
```

没有 GPIO16→RST 时必须外部复位。

## 三、主循环监护

默认整轮未覆盖时间阈值：

```ini
-DESP8266BASE_WDT_TIMEOUT_MS=2500
```

范围限制为 1000～3000ms。`Esp8266Base::handle()` 在轮首推进 heartbeat，并用阶段标记区分 Config、WiFi、Journal、NTP、Web、OTA 和 MQTT。NTP/Web/MQTT 的已知有界阻塞仍通过 `account()` 计入宽限；超过各自上限的部分视为异常。

独立冻结阈值默认 90 秒：

```ini
-DESP8266BASE_WDT_STALL_TIMEOUT_MS=90000
```

`os_timer` 每秒比较 heartbeat。主循环即使在 SDK/lwIP/TLS 内持续 `yield()`，heartbeat 不推进仍会被发现。不 yield 的永久循环继续由 ESP8266 SDK 软件/硬件 WDT 兜底。

## 四、安全回调

执行器项目必须在 `Esp8266Base::begin()` 前注册：

```cpp
Esp8266BaseWatchdog::setSafetyCallbacks(emergencyStop, restartGuard);
```

- `emergencyStop` 必须是无分配、无日志、无文件和无网络 I/O 的极短函数；冻结时优先把输出置于安全状态。
- `restartGuard` 用于普通网络恢复重启。执行器正在运行时应返回 false；网络失联不得提前中止已接受任务。
- 主循环冻结属于安全故障，会先执行 `emergencyStop`，不受普通网络运行门禁保护。

异步回调不能调用 `delay()`、`yield()`、LittleFS、日志、MQTT、`String` 或动态分配。

## 五、恢复重启预算

- 每次自动恢复重启后，60 分钟内不允许再次自动重启；
- 连续自动恢复重启最多 2 次；application-ready（传输、必需订阅和初始证据均完成）稳定 30 分钟后只打断这条连续链；
- UTC 可信时，滚动 24 小时窗口内最多 2 次；UTC 未知时只执行 60 分钟冷却与连续链约束；
- 没有固件 pending 标记的上电/外部/维护启动视为人工介入并打断连续链，但 RTC 尚存时不抹掉可信 24 小时计数；断电导致 RTC 丢失不计为自动重启；
- 预算耗尽时先保证输出安全，写入 denied RTC 证据，不形成重启风暴。

网络长期失败由 MQTT 恢复阶梯调用：普通重连 → radio reset → 在业务 guard 允许时请求 MCU 重启。

## 六、RTC 与计数

`eb_wdt_count` 保存由 loop stall 触发的累计恢复次数。异步监护回调只写 RTC user memory，不写 LittleFS 或 Config；下一次正常启动再补写 Flash。

库保留：

| RTC word | 字节数 | 用途 |
|---:|---:|---|
| 64-70 | 28B | magic、WDT count、连续恢复次数、原因/阶段/拒绝原因、24 小时窗口起点与次数、checksum |

业务不得复用 64～70。

`lastRecoveryCause()`、`lastStallPhase()`、`lastRecoveryWasDenied()` 与 `lastRecoveryDecision()` 返回上次 RTC 胶囊；`consecutiveRecoveryRestarts()` 与 `recoveryRestartsInWindow()` 返回两套独立预算。它们与 SDK reset reason 是两套证据，不能互相替代。

## 七、OTA

OTA 上传期间库自动 `pause()`，所有失败路径必须 `resume()`。成功 OTA 在受控 MQTT shutdown 完成后保持暂停并由 OTA 流程重启。`resume()` 会推进 heartbeat，避免把暂停时间误判为冻结。

## 八、验收

- 注入普通慢调用：返回后能记录 phase 和 overrun；
- 注入持续 `yield()` 的永久等待：90 秒内执行安全回调并重启；
- 连续触发时分别验证 60 分钟冷却、30 分钟 application-ready 断链、可信 UTC 24 小时最多 2 次及未知时间分支；
- 执行器运行时网络恢复重启被 guard 拒绝，但本地截止继续；
- RTC 原因、SDK reset reason 和 Journal boot 记录能够关联；
- OTA、Web、TLS 压测无误触发。

启用RecordStore时，正常deepSleep前保存释放检查点并暂停访问；失败不阻断安全休眠，恢复后允许幂等重放。看门狗/异常复位不新增Flash写入。应用直接调用ESP.restart/system_restart前应自行调用Store::prepareMaintenance，不能假设库能拦截任意复位。
