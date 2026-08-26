# AGENTS.md

本文件是 Codex / 代码代理在本仓库工作的项目级规范。详细行为以 `README.md` 和 `docs/` 为准；如果本文件与代码或文档不一致，先核实事实并提出修正方案，不要凭印象修改。

## 1. 项目边界

- Esp8266Base 是 **ESP8266-only** Arduino 基础库，只服务 ESP8266 / ESP-12F。
- 禁止新增 ESP32 支持、`#ifdef ESP32`、跨芯片 HAL、事件总线、通用调度框架。
- 优先级：稳定、低 RAM、可诊断、易集成。
- 当前按 1.0.0 发布标准维护；不为旧方案保留兼容包袱，除非用户明确要求重新评估兼容策略。

## 2. 工作原则

- 业务项目反馈必须先判断是否属于本库的通用问题或基础能力。
- 属于本库职责时，在本库中根治；不要求业务项目绕开。
- 不属于本库职责时，说明边界、原因和业务侧处理方式。
- 新需求、bug、优化不做临时补丁、局部绕路或旧行为兼容层。
- 实施前必须先读相关代码和文档；行为变化必须同步 README、对应 docs、API 参考和示例。

## 3. 常用命令

```bash
tools/test_all.sh
tools/test_all.sh --all-envs
pio run -e esp12f
cd examples/full_demo && pio run -e esp12f
```

硬件上传、串口监视、OTA、deep sleep、GPIO 按钮验收属于人工或需用户明确确认的操作。上传示例：

```bash
pio run -e esp12f -t upload --upload-port /dev/cu.usbserial-120
```

macOS 上 `pio device monitor` 可能有 termios 问题；串口监视优先用 Python。

## 4. 测试原则

- `tools/test_all.sh` 是默认测试入口。
- 默认测试不烧录、不访问串口、不依赖 WiFi 或 ESP12F 在线。
- 默认测试覆盖 `git diff --check`、静态一致性、轻量逻辑检查、根项目和全部 examples 的 `esp12f` 编译。
- 扩展检查运行 `tools/test_all.sh --all-envs`。
- 新增可自动验证的规则，优先放进 `tools/check_static.sh` 或 `tools/check_logic.py`。

## 5. 架构硬约束

- 库模块使用静态类；不实例化，不使用虚函数，不使用 `new` / `malloc`。
- 入口固定为 `Esp8266Base::begin()` 和 `Esp8266Base::handle()`。
- 初始化顺序和 `handle()` 顺序以 `docs/02_architecture.md` 为准；NTP 和 mDNS 只在 WiFi STA 连接后由 `handle()` 触发。
- 模块依赖必须单向，禁止循环依赖；不要让底层模块依赖业务代码。

## 6. RAM 与实现硬约束

- RAM 预算以 `docs/04_memory_budget.md` 为唯一权威来源。
- 禁止单个全局/静态缓冲超过 512B；Web handler 临时缓冲优先 <= 64B。
- HTML 必须放 `PROGMEM`，不得放 DRAM。
- Web 必须用 `sendContent_P()` / `sendChunk()` 流式输出，不拼整页 `String`。
- 禁止 `std::function`、STL 容器、递归。
- 模块全局状态不得保存 `String`；使用固定长度 `char[]`。
- 新增模块或明显增加常驻 RAM 时，必须同步 `docs/04_memory_budget.md`。

## 7. 关键行为规范

- 配置存储使用 LittleFS `/cfg_<key>`；库保留 key 以 `docs/05_config_storage.md` 为准，必须使用 `eb_` 前缀，业务项目不得复用。
- 配置写入必须写前比较；高频计数使用 deferred；正常重启、deep sleep、OTA 成功前 flush 配置和文件日志；WDT 超时异常路径只写 RTC 标记，不写 LittleFS。
- LittleFS 挂载失败默认不格式化，只有 `ESP8266BASE_CFG_FORMAT_ON_FAIL=1` 才允许。
- WiFi 无凭证进入 AP；有凭证连接失败时保持 STA 持续重连，不自动进 AP。这是面向家庭设备的自恢复设计：常见故障是路由器或上游 WiFi 暂时不可用，持续重试可在路由器恢复后自动回连；自动切到 AP 会退出正常 STA 恢复路径。除非用户明确改变产品策略，否则不得把“不自动进 AP”作为缺陷反复提出，也不得增加超时转 AP 或 AP/STA 并行兜底；重新配网必须由清除凭证或恢复出厂等显式操作触发。
- Web 表单必须防重复提交；危险操作必须二次确认；POST 成功后用 303 重定向。
- OTA 使用 `Update.begin(ESP.getFreeSketchSpace())`，不要使用 ESP32 的 `UPDATE_SIZE_UNKNOWN`。
- deep sleep 后 Web 不响应是正常行为；唤醒依赖 GPIO16→RST 或外部复位。
- WiFi 密码、Web Auth 密码和配置审计值明文日志是当前个人本机/家庭项目为方便现场调试明确接受的设计取舍，不按 bug 处理；在复用本库的同边界业务项目评审中不要重复列为安全问题。仍不得把含敏感值的日志提交、公开或外发；只有部署边界变为生产、共享设备、不可信局域网或用户明确要求改变安全策略时，才重新评估脱敏与传输保护。

## 8. 文档、分区与发布

- 文档使用中文；API、宏、路径保持英文原文。
- 文档描述当前正确行为，不记录历史过程。
- ESP-12F 默认使用 `partitions/esp8266-4mb-2mfs.ld`，并必须保留 `INCLUDE "local.eagle.app.v6.common.ld"`。
- 涉及 ESP8266 Arduino Core、PlatformIO、LittleFS、Update、硬件引脚或协议细节时，优先依据权威资料；无法核实时明确说明不确定性和风险。
- 发布前默认运行 `tools/test_all.sh`；需要扩展验证时运行 `tools/test_all.sh --all-envs`。
