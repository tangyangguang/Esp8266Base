# Esp8266Base 维护者指南

> 版本：1.0.0  
> 读者：库维护者、贡献者、业务项目高级开发者

---

## 一、维护原则

- ESP8266 only，不添加 ESP32 分支。
- RAM 优先，功能必须服从 ESP8266 资源边界。
- 模块均为静态类，不实例化，不使用虚函数。
- 默认行为保持轻量，诊断能力通过显式开关启用。
- 文档和代码必须同步，文档按当前代码事实编写。

---

## 二、模块依赖

依赖方向只能从协调器到模块，避免反向耦合：

```text
Esp8266Base
  ├─ Log
  ├─ Config
  ├─ WiFi
  ├─ Web → OTA
  ├─ NTP
  ├─ mDNS
  ├─ Sleep
  └─ Watchdog
```

不要让业务模块直接绕过 Web 路由表注册内置行为。不要让底层模块依赖业务代码。

---

## 三、RAM 与代码规则

硬规则：

- 不使用 STL 容器。
- 不使用 `std::function`。
- 不拼接整页 `String`。
- HTML 放 PROGMEM。
- 单个静态缓冲不超过 512B。
- Web handler 临时缓冲优先 <= 64B。
- 新增模块或新增常驻状态必须更新 `docs/04_memory_budget.md`。

---

## 四、文件系统规则

Config 正式配置写入必须使用安全策略：

```text
写 tmp -> 读回校验 -> 旧文件改 bak -> tmp 提交为正式文件 -> 删除 bak
```

禁止直接对正式配置文件 `open("w")` 截断写入。

日志文件不是数据库。允许极端情况下丢少量日志，但不能让 FileLog 或 `/logs` 长期失效。轮转或追加异常时优先恢复当前写入能力。

---

## 五、Web 规则

- 内置页面必须 Basic Auth，除非明确是健康探测。
- 表单必须使用 `once(form)` 防重复提交。
- 危险操作必须有 `confirm()`。
- POST 成功后优先 `303 See Other` 回 GET。
- 自定义页面通过 `addPage(path, title, handler)`，API 通过 `addApi()`。
- 业务主界面用 `setHomePath()` / `setHomeMode()` 配置；不要让业务项目手写根路径重定向或隐藏基础库导航。
- 不直接使用 `server().on()` 注册业务路由。

---

## 六、日志与审计规则

- WiFi 密码、Web Auth 密码明文日志是设计要求，不按 bug 处理。
- 配置审计不脱敏，直接输出 key/value。
- FileLog 只支持 OFF/WARN/INFO；WARN 模式写 WARN/ERROR，INFO 模式写 INFO/WARN/ERROR。
- NTP 同步后必须输出实际时间和 boot time 映射。
- 启动会话必须有分割线、boot_count、`boot_reason`、中文 `boot_desc`、firmware、version、人性化 free heap。

---

## 七、修改文档要求

任何功能变化都要同步：

- README 摘要或导航
- `docs/00_user_guide.md`
- 对应能力域文档
- `docs/03_api_reference.md`
- 如涉及 RAM，更新 `docs/04_memory_budget.md`

不要在发布文档中记录历史过程。文档应描述“当前正确行为”。

---

## 八、发布检查清单

构建：

```bash
tools/test_all.sh
```

`tools/test_all.sh` 不烧录、不访问串口、不要求 ESP12F 在线；它覆盖 `git diff --check`、静态一致性检查、轻量逻辑检查、根项目 `esp12f` 编译和全部示例 `esp12f` 编译。需要额外验证非发布板型时运行 `tools/test_all.sh --all-envs`。

硬件：

- 烧录 full_demo。
- 无凭证时进入 AP。
- 配网后 STA 连接成功。
- Web 首页可访问。
- OTA 页面可上传。
- NTP 输出 `time_synchronized` 和 `time_mapping`。
- `/logs` 显示 4 段文件状态。
- System 页面可切换 FileLog 模式；System 页面可通过 `/logs/clear` 清空日志。
- GPIO0 长按清配置有效。
- Watchdog 清零和 deep sleep 页面有确认。

文档搜索：

```bash
rg -n 'eb_wifi_ssid|eb_wifi_pass|eb_boot_count|Esp8266BaseFileLog|rotateFiles|/logs/clear' README.md docs
```

无前缀 key 不应作为库保留 key 出现在文档或代码中。
