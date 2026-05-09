# AGENTS.md

本文件是 Codex / 代码代理在本仓库工作的核心规范。详细行为以 `README.md` 和 `docs/` 为准；代码、文档、测试和方案讨论都必须遵守本文件。

## 1. 项目立场

- Esp8266Base 是 **ESP8266-only** Arduino 基础库，只服务 ESP8266 / ESP-12F。
- 禁止新增 ESP32 支持、`#ifdef ESP32`、跨芯片 HAL、事件总线、通用调度框架。
- 优先级：稳定、低 RAM、可诊断、易集成。
- 当前按 1.0.0 对外发布标准维护；不为旧方案保留兼容包袱。

## 2. 反馈处理原则

- 业务项目反馈必须先判断是否属于本库的通用问题或基础能力。
- 属于本库职责时，在本库中根治，不要求业务项目绕开。
- 不属于本库职责时，说明边界、原因和业务侧处理方式。
- 新需求、bug、优化都不做临时补丁、局部绕路、旧行为兼容层。
- 即使影响大量代码、API、示例和文档，也只做当前最优方案和最佳实践。
- 实施前必须先读相关代码和文档，不能凭印象修改。

一句话：**先定边界，再做根治；拒绝补丁式修修补补。**

## 3. 常用命令

```bash
tools/test_all.sh
tools/test_all.sh --all-envs
pio run -e esp12f
cd examples/full_demo && pio run -e esp12f
pio run -e esp12f -t upload --upload-port /dev/cu.usbserial-120
```

串口监视优先用 Python；macOS 上 `pio device monitor` 可能有 termios 问题。

## 4. 测试原则

- `tools/test_all.sh` 是默认测试入口。
- 默认测试不烧录、不访问串口、不依赖 WiFi 或 ESP12F 在线。
- 默认测试覆盖格式检查、静态一致性、轻量逻辑检查、根项目和全部 examples 的 `esp12f` 编译。
- 硬件烧录、配网、OTA、deep sleep、GPIO 按钮属于人工或单独硬件验收。
- 新增可自动验证的规则，优先放进 `tools/check_static.sh` 或 `tools/check_logic.py`。

## 5. 架构硬约束

- 所有模块使用静态类；不实例化，不使用虚函数，不使用 `new` / `malloc`。
- 入口固定为 `Esp8266Base::begin()` 和 `Esp8266Base::handle()`。
- 初始化顺序：`Log → Sleep → Config(LittleFS) → WiFi → Watchdog → Web → OTA → logDiagnostics()`。
- `handle()` 顺序：`Config → Log → WiFi → NTP/mDNS trigger → NTP → mDNS → Web → Watchdog`。
- NTP 和 mDNS 只在 WiFi STA 连接后由 `handle()` 触发。
- 模块依赖必须单向，禁止循环依赖。

## 6. RAM 与实现硬约束

- 库自身全局静态 RAM 目标控制在 2.5KB 内。
- 禁止单个全局/静态缓冲超过 512B。
- HTML 必须放 `PROGMEM`，不得放 DRAM。
- Web 必须用 `sendContent_P()` / `sendChunk()` 流式输出，不拼整页 `String`。
- 禁止 `std::function`、STL 容器、递归。
- 模块全局状态不得保存 `String`；使用固定长度 `char[]`。
- 新增模块或明显增加 RAM 时，必须同步 `docs/04_memory_budget.md`。
- 目标 free heap：正常联网 Web 空闲 >= 24KB；Web 页面 >= 18KB；OTA 中 >= 12KB；AP 配网 >= 18KB。

## 7. 关键行为规范

- 配置存储使用 LittleFS `/cfg_<key>`；库保留 key 必须使用 `eb_` 前缀，业务项目不得复用。
- 配置写入必须写前比较；高频计数使用 deferred；重启、deep sleep、OTA 成功、WDT 重启前 flush 配置和文件日志。
- LittleFS 挂载失败默认不格式化，只有 `ESP8266BASE_CFG_FORMAT_ON_FAIL=1` 才允许。
- 日志必须可读、字段清晰；大字节数使用 KB/MB；启动必须有 boot session 分割线。
- WiFi 密码、Web Auth 密码和配置审计值按设计明文输出，不视为 bug。
- NTP 同步后必须输出实际时间、uptime、boot time、millis 映射，并切换日志时间戳。
- 文件日志默认关闭；默认文件等级 WARN；WARN/ERROR 立即写文件；低于 WARN 时才启用低优先级缓存。
- WiFi 无凭证进入 AP；有凭证连接失败时保持 STA 持续重连，不自动进 AP。
- Web 表单必须防重复提交；危险操作必须二次确认；POST 成功后用 303 重定向。
- OTA 使用 `Update.begin(ESP.getFreeSketchSpace())`，不要使用 ESP32 的 `UPDATE_SIZE_UNKNOWN`。
- deep sleep 后 Web 不响应是正常行为；唤醒依赖 GPIO16→RST 或外部复位。

## 8. 文档、分区与发布

- 代码行为变化必须同步文档；文档描述当前正确行为，不记录历史过程。
- 文档使用中文；API、宏、路径保持英文原文。
- ESP-12F 默认使用 `partitions/esp8266-4mb-2mfs.ld`，并必须保留 `INCLUDE "local.eagle.app.v6.common.ld"`。
- 默认发布检查运行 `tools/test_all.sh`，扩展检查运行 `tools/test_all.sh --all-envs`。
