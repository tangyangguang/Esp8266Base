# CHANGELOG.md

本文件记录从 **2026-05-06** 起 Esp8266Base 的新增能力、行为变化、重要优化和对业务项目有影响的使用建议。

目标读者是使用本基础库开发业务固件的项目。业务项目应优先阅读本文件，了解库最近提供了哪些新能力、默认行为有什么变化、是否需要调整自己的代码。

## 维护规则

- 只记录从 2026-05-06 起发生的变化，不补写早期历史。
- 每次新增功能、修复通用 bug、改变默认行为、调整 API、改变配置 key、影响日志/存储/WiFi/Web/OTA/Sleep/Watchdog 行为时，都必须更新本文件。
- 记录要面向使用者，说明“业务项目需要知道什么”，不要写开发过程流水账。
- 如果变更只影响内部实现且业务项目不需要感知，可以不记录。
- 详细 API 和完整行为仍以 `README.md` 和 `docs/` 为准。

## 记录格式

每条记录使用以下格式：

```markdown
## YYYY-MM-DD

### 新增
- ...

### 优化
- ...

### 修复
- ...

### 行为变化 / 使用建议
- ...
```

没有内容的小节可以省略。

## 2026-05-08

### 修复

- 修复 `ESP8266BASE_USE_OTA=1` 且 `ESP8266BASE_USE_WATCHDOG=0` 时 OTA 模块引用 Watchdog pause/resume 导致链接失败的问题。
- 修复 `ESP8266BASE_USE_SLEEP=1` 且 `ESP8266BASE_USE_WATCHDOG=0` 时 deep sleep 路径引用 Watchdog pause 导致链接失败的问题。
- Web Auth 日志不再输出明文密码；只记录来源、长度和结果。`eb_web_pass` 在 Config 读写审计日志中也会显示为 `(redacted)`。
- Web 内置导航和系统首页对应用配置的路径、标题、日志路径做更严格的 HTML 输出处理，应用路由路径限制为 `/` 开头且仅包含字母、数字、`/`、`-`、`_`、`.`。
- `Esp8266BaseConfig::flush()` 现在会聚合返回写入结果；任一 pending 写入失败时返回 `false`，并保留失败项，避免重启或 deep sleep 前静默丢失 deferred 配置。

### 优化

- 精简项目 `AGENTS.md` 到 100 行以内，保留核心规则并将细节继续交给 `README.md` 和 `docs/`。

## 2026-05-07

### 新增

- 新增内置 `/auth` 页面：可在 Web 管理页中修改 Basic Auth 密码。页面要求 Basic Auth，校验当前密码、新密码非空、长度不超过 23、确认一致；保存成功后写入 `eb_web_pass` 并立即使用新密码。
- 新增 `Esp8266BaseWeb::setDefaultAuth(user, pass)`，用于设置业务代码默认认证值。认证优先级为编译期宏默认值、`setDefaultAuth()`、设备持久化 `eb_web_user` / `eb_web_pass`。
- 新增 `Esp8266BaseWebBuiltinLabel::AUTH`，内置导航默认显示 `Auth`，可通过 `setBuiltinLabel()` 改名。

### 优化

- 内置页面整体内容容器默认在浏览器中水平居中，正文和日志内容仍保持左对齐。
- 系统首页改为轻量分组展示 Network、Device、Time 信息，新增 SSID、RSSI、MAC、hostname、固件信息、库级 boot count、NTP 状态、当前时间和 Boot time。
- 首页 `Uptime` 改为人性化格式并保留秒级精度；`Boot time` 在 NTP 同步后显示到秒。
- `/logs` 页面改为单文件段展示：默认显示最新 `current-0`，顶部提供 `current-0`、`history-1`、`history-2`、`history-3` 标签切换，避免一次输出全部轮转日志导致页面过大。
- WiFi STA 首连增加 `WL_DISCONNECTED` 卡住恢复策略：默认 7s 无进展会记录 `station_connect_stuck_restarting` 并重启本轮连接，避免每次都白等 20s 连接观察窗口。
- WiFi 前 3 次快速重试间隔从 5s 调整为 2s；如果 stuck restart 后仍然卡在 `WL_DISCONNECTED`，会记录 `station_connect_stuck_retrying` 并直接进入快速重试，不再继续等满 20s。
- `wifi_retry_policy` 增加 `stuck_disconnected` 字段，便于确认当前构建的首连卡住恢复阈值。
- `file_sink_enabled` 启用日志拆成两行，避免 INFO 文件日志配置较长时出现 `buffer_size=` 这类截断字段。
- 启动会话日志拆分为多行，避免单行过长：分割线、`boot_session_start`、`boot_reason`/`boot_desc`、固件信息和 `free_heap` 分别输出。
- 启动原因字段统一为 `boot_reason`，并增加中文说明字段 `boot_desc`：`power-on`、`deep-sleep`、`soft-restart`、`wdt-reset`、`unknown` 都有固定中文解释。
- 启动会话中的 `free_heap` 改为人性化格式，例如 `39.8 KB`。

### 行为变化 / 使用建议

- 删除旧 `Esp8266BaseWeb::setAuth(user, pass)` API。业务项目应改用 `setDefaultAuth(user, pass)`，并理解它只设置默认值，不会覆盖设备上已经保存的 `eb_web_pass`。
- `clearAll()` 后会删除 `eb_web_pass`，Web Auth 恢复为 `setDefaultAuth()` 或编译期默认密码。
- 业务项目如果上电后经常看到第一轮 `station_connect_timeout elapsed=20000ms status=WL_DISCONNECTED`，升级后应观察是否改为约 7s 出现 `station_connect_stuck_restarting`，常见场景下联网时间会缩短。
- 业务项目如果解析启动会话日志，应使用 `boot_reason` 替代旧的 `reset_reason` 字段；本库不输出 ESP32 风格的 `wake_reason`，也不会输出 `undefined`。

## 2026-05-06

### 新增

- 新增 Web 首页与导航模型：`setDeviceName()`、`setHomePath()`、`setHomeMode()`、`setSystemNavMode()`、`setBuiltinLabel()`，业务项目可将 `/` 配置为业务首页，并将基础库能力降级为系统工具入口。
- 新增 `Esp8266BaseWebHomeMode`：默认系统首页、业务首页优先、融合首页。`FUSED_HOME` 下 `/` 返回 `303` 到业务首页，`/esp8266base` 保留为基础库系统首页；`APP_HOME_FIRST` 下 `/` 和 `/esp8266base` 都返回 `303` 到业务首页。
- 新增 `Esp8266BaseWebSystemNavMode`：系统入口可显示在顶部导航、底部导航或 footer 紧凑工具区。`FOOTER_COMPACT` 会把系统入口与 `Free heap` 放在同一 footer 区域，小字号、可换行，适合业务页面作为主界面。
- 新增 `addPage(path, title, handler)` 和 `addNavItem(path, title)`，业务页面可以声明导航标签，不再只能显示路径名。
- 新增必要自动化测试入口 `tools/test_all.sh`，默认执行无硬件依赖的测试：格式检查、静态一致性检查、轻量逻辑检查、根项目和全部 examples 的 `esp12f` 编译。
- 新增 `tools/check_static.sh`，用于检查 ESP8266-only 约束、`eb_*` 保留 key、示例日志等级、文档关键接口引用。
- 新增 `tools/check_logic.py`，用于检查 `formatBytes()`、文件日志缓存宏规则、WiFi 重试策略、日志轮转路径规则。

### 优化

- Config deferred 写入新增 `ESP8266BASE_CFG_DEFERRED_FLUSH_INTERVAL_MS`，默认 5000ms。`setIntDeferred()` / `setBoolDeferred()` 高频更新同一个 key 时只覆盖内存 pending 值，`handle()` 到达间隔后最多刷 1 条，降低业务计数器持续变化对 Flash 寿命的影响；`flush()` 仍会在 deep sleep、重启、OTA 成功前强制落盘。
- 根项目 `esp12f` 烟测配置的 WiFi 快速重试间隔跟随库默认值，避免 full_demo 烧录后首轮联网失败时额外等待。
- STA 连接前新增 `ESP8266BASE_WIFI_STA_SETTLE_MS`，默认 150ms，用于在切换 STA/断开旧状态后等待 ESP8266 WiFi SDK 状态稳定，降低上电后首轮连接停在 `WL_DISCONNECTED` 的概率。
- WiFi 启动时新增 `wifi_retry_policy` 日志，输出当前连接观察窗口、STA 稳定等待、快速重试间隔、快速重试次数和慢速重试间隔，便于业务项目判断是否被构建宏覆盖。
- 简化并中文化 `AGENTS.md`，将其定位为代理工作核心规范，详细说明移到 `README.md` 和 `docs/`。

### 行为变化 / 使用建议

- 业务项目日常验证基础库变更时，优先运行 `tools/test_all.sh`。该脚本不烧录、不访问串口、不要求 ESP12F 在线。
- 如果需要额外验证 `nodemcuv2` 编译环境，运行 `tools/test_all.sh --all-envs`。
- 硬件烧录、WiFi 配网、OTA、deep sleep、GPIO 按钮仍属于人工或单独硬件验收，不纳入默认自动化测试。
- 推荐业务项目在 `Esp8266Base::begin()` 前配置 `setHomePath()` / `setHomeMode()`，在 `begin()` 后用 `addPage(path, title, handler)` 注册业务首页；不要通过 CSS 隐藏基础库导航、手写 `/` 重定向或绕过 `sendHeader()` / `sendFooter()` 输出页面框架。
