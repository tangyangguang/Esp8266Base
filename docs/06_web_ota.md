# Esp8266Base Web 与 OTA

> 版本：1.0.0  
> 模块：`Esp8266BaseWeb` / `Esp8266BaseOTA`

---

## 一、能力范围

Web 模块提供轻量管理页面、Basic Auth、内置改密、自定义页面/API 注册、业务导航/首页配置和流式 HTML 输出。OTA 模块基于 Web 模块注册 `POST /ota`，提供浏览器固件上传。

ESP8266 Web 活跃时 free heap 有限，本库固定自定义路由上限：

| 类型 | 默认上限 | 编译宏 |
|---|---:|---|
| 应用页面 | 4 | `ESP8266BASE_WEB_MAX_APP_PAGES` |
| 应用 API | 6 | `ESP8266BASE_WEB_MAX_APP_APIS` |
| 路径长度 | 小于 24 字符 | 固定限制 |
| 路径字符集 | 字母、数字、`/`、`-`、`_`、`.` | 固定限制 |

---

## 二、内置路由

| 路由 | 方法 | 认证 | 说明 |
|---|---|---|---|
| `/` | GET | Basic Auth | 首页；可配置为跳转业务首页 |
| `/esp8266base` | GET | Basic Auth | 基础库系统首页；融合模式下作为系统入口保留 |
| `/wifi` | GET | Basic Auth | WiFi 配置页，回显 SSID/密码 |
| `/wifi` | POST | Basic Auth | 保存 WiFi 凭证，提交后 303 回 GET |
| `/auth` | GET | Basic Auth | 修改 Web Basic Auth 密码 |
| `/auth` | POST | Basic Auth | 校验当前密码并保存 `eb_web_pass`，提交后 303 回 GET |
| `/ota` | GET | Basic Auth | OTA 上传页，带进度显示 |
| `/ota` | POST | Basic Auth | 固件上传，由 OTA 模块处理 |
| `/logs` | GET | Basic Auth | 查看文件日志状态、文件等级、缓存状态和单个日志段内容 |
| `/logs/clear` | POST | Basic Auth | 清空文件日志；入口在 Tools 页面 |
| `/reboot` | GET | Basic Auth | Tools 页面，包含清除文件日志和重启设备 |
| `/reboot` | POST | Basic Auth | flush 配置后重启 |
| `/health` | GET | 无 | JSON 健康信息 |

管理操作都需要 Basic Auth。`/health` 用于轻量状态探测，不要求认证。

应用路由路径必须以 `/` 开头，长度小于 24 字符，并且只允许字母、数字、`/`、`-`、`_`、`.`。不符合规则时 `addPage()` / `addApi()` 返回 `false` 并输出 WARN 日志。内置导航和系统首页会对应用提供的路径、标题和日志路径做 HTML 输出转义。

---

## 三、业务首页与导航

默认不配置时，`/` 和 `/esp8266base` 都显示 Esp8266Base 系统首页，顶部导航包含内置系统页和应用页面。

系统首页以轻量分组展示当前设备状态：

| 分组 | 字段 |
|---|---|
| Network | Hostname、WiFi 状态、SSID、IP、RSSI、MAC |
| Device | Firmware、Version、Boot count、Chip ID、CPU、Flash、Sketch、OTA free |
| Time | Uptime、NTP 状态、当前时间、Boot time |

`Uptime` 使用人性化格式并保留秒级精度。`Boot time` 在 NTP 同步后显示为 `YYYY-MM-DD HH:MM:SS`，同步前显示 `-`；未启用 NTP 时显示 `NTP: disabled`。

业务项目希望业务页面成为主界面时，在 `Esp8266Base::begin()` 前配置首页和导航模型，在 `begin()` 后注册页面：

```cpp
Esp8266BaseWeb::setDeviceName("Sensor Node");
Esp8266BaseWeb::setHomePath("/sensor");
Esp8266BaseWeb::setHomeMode(Esp8266BaseWebHomeMode::FUSED_HOME);
Esp8266BaseWeb::setSystemNavMode(Esp8266BaseWebSystemNavMode::FOOTER_COMPACT);
Esp8266BaseWeb::setBuiltinLabel(Esp8266BaseWebBuiltinLabel::HOME, "Status");
Esp8266BaseWeb::setBuiltinLabel(Esp8266BaseWebBuiltinLabel::WIFI, "Network");
Esp8266BaseWeb::setBuiltinLabel(Esp8266BaseWebBuiltinLabel::OTA, "Update");
Esp8266BaseWeb::setBuiltinLabel(Esp8266BaseWebBuiltinLabel::AUTH, "Auth");
Esp8266BaseWeb::setBuiltinLabel(Esp8266BaseWebBuiltinLabel::REBOOT, "Tools");

Esp8266Base::begin();
Esp8266BaseWeb::addPage("/sensor", "Sensor", handleSensorPage);
```

首页模式：

| 模式 | `/` | `/esp8266base` |
|---|---|---|
| `DEFAULT_SYSTEM_HOME` | 系统首页 | 系统首页 |
| `APP_HOME_FIRST` | `303` 到业务首页 | `303` 到业务首页 |
| `FUSED_HOME` | `303` 到业务首页 | 系统首页 |

系统导航模式：

| 模式 | 说明 |
|---|---|
| `TOP_NAV` | 基础功能入口在顶部导航，适合基础库独立使用 |
| `BOTTOM_NAV` | 基础功能入口在页面内容下方，降低视觉层级 |
| `FOOTER_COMPACT` | 基础功能入口在 footer 中与 `Free heap` 同区，小字号、可换行，适合业务应用主界面 |

`FOOTER_COMPACT` 不输出额外工具标题，不使用 `details/summary`，也不显示展开图标。桌面端状态入口和 `Free heap` 尽量同一行；窄屏下自然换行，避免横向滚动。

---

## 四、Web Auth 默认值与改密

认证配置分三层：

| 优先级 | 来源 | 说明 |
|---:|---|---|
| 1 | `ESP8266BASE_WEB_AUTH_USER/PASS` | 编译期默认用户名和密码 |
| 2 | `Esp8266BaseWeb::setDefaultAuth(user, pass)` | 业务代码默认值，必须在 `Esp8266Base::begin()` 前设置 |
| 3 | `eb_web_user` / `eb_web_pass` | 设备持久化值，优先级最高 |

`setDefaultAuth()` 不是强制覆盖用户保存的密码。设备上已经保存 `eb_web_pass` 时，启动后会优先使用保存值。Web 已启动后再调用 `setDefaultAuth()` 会被忽略。

`/auth` 页面用于修改密码，不修改用户名。页面字段包含当前密码、新密码、确认新密码；新密码不能为空，最长 23 字符，确认必须一致。保存成功后立即更新运行时密码并写入 `eb_web_pass`，随后 `303` 回 `/auth?saved=1`。浏览器如果仍缓存旧 Basic Auth，可能会在跳转后重新弹出认证框，这是预期行为。Web Auth 改密成功和失败路径都会把当前密码、新密码、确认密码等值明文写入日志；`eb_web_pass` 的 Config 审计也按明文输出。这是个人项目为了调试观察保留的设计。

本库不提供 CSRF token 机制；Basic Auth 会被浏览器自动带到同源请求中。Web 管理页、OTA、危险 POST 操作默认面向可信局域网和个人调试环境使用。

`clearAll()` 删除所有 `/cfg_*` 配置后，Web Auth 恢复为 `setDefaultAuth()` 设置的默认值；如果业务代码没有设置，则恢复为编译期宏默认值。

---

## 五、表单防重复提交

`sendHeader()` 内置轻量 JS：

```js
function once(f) {
  if (f.dataset.busy) return false;
  f.dataset.busy = 1;
  var b = f.querySelector('[type=submit]');
  if (b) b.disabled = true;
  return true;
}
```

内置表单会使用 `once(this)`，危险操作还会加 `confirm()`。POST 成功后使用 `303 See Other` 跳回 GET 页面，避免浏览器刷新重复提交。

---

## 六、自定义页面/API

必须在 `Esp8266Base::begin()` 后注册：

```cpp
void handlePage() {
    if (!Esp8266BaseWeb::checkAuth()) return;
    Esp8266BaseWeb::sendHeader();
    Esp8266BaseWeb::sendContent_P(PSTR("<h2>Page</h2>"));
    Esp8266BaseWeb::sendFooter();
}

void setup() {
    Esp8266Base::begin();
    Esp8266BaseWeb::addPage("/page", "Page", handlePage);
}
```

Handler 必须是普通函数或无捕获 lambda。不要直接调用 `server().on()` 注册业务路由，否则会绕过静态路由表和请求诊断。

---

## 七、RAM 安全写法

规则：

- 静态 HTML 放 PROGMEM。
- 动态 HTML 用 `snprintf()` 到小栈缓冲，再 `sendChunk()`。
- 不拼接整页 `String`。
- 单个临时缓冲建议不超过 64B，复杂 JSON 不超过 128B。

示例：

```cpp
static const char PAGE[] PROGMEM =
    "<h2>Control</h2>"
    "<form method='post' action='/api/ctrl' onsubmit='return once(this)'>"
    "<input name='value'>"
    "<input type='submit' value='Save'>"
    "</form>";

void handleCtrlPage() {
    if (!Esp8266BaseWeb::checkAuth()) return;
    Esp8266BaseWeb::sendHeader();
    Esp8266BaseWeb::sendContent_P(PAGE);
    char buf[48];
    snprintf(buf, sizeof(buf), "<p>Heap: %u</p>", ESP.getFreeHeap());
    Esp8266BaseWeb::sendChunk(buf);
    Esp8266BaseWeb::sendFooter();
}
```

---

## 八、OTA 行为

启用条件：

```ini
-DESP8266BASE_USE_WEB=1
-DESP8266BASE_USE_OTA=1
```

访问：

```text
http://<device-ip>/ota
```

行为：

- `GET /ota` 需要 Basic Auth。
- `POST /ota` 也需要 Basic Auth。
- 上传页面使用 XMLHttpRequest 显示百分比和字节数。
- 日志输出 `upload_started`、`upload_progress`、`upload_finished`、`upload_success` / `upload_failed` / `upload_aborted`，包含上传字节数、`elapsed`、`average_speed`、free heap 等诊断字段；进度百分比基于 multipart request length 近似，完成日志以真实固件字节数为准。
- OTA 上传期间暂停 Watchdog，上传完成后恢复。
- 使用 `Update.begin(ESP.getFreeSketchSpace())`。
- 成功后 flush 响应并重启。
- 当前不计算 SHA256；OTA 依赖 `Update.write()` / `Update.end(true)` 的写入与镜像校验结果决定是否接受固件。

如果出现 `Unauthorized`，先确认浏览器当前会话已经通过 Basic Auth，或重新打开 `/ota` 输入用户名密码。

---

## 九、失败模式

| 现象 | 常见原因 | 处理 |
|---|---|---|
| 页面加载慢 | ESP8266 同时处理 WiFi/Web/LittleFS，或客户端较慢 | 查看 `slow_request` 日志 |
| 表单重复提交 | 用户连续点击或刷新 POST | 使用 `once(this)` 和 POST 后 303 |
| OTA Unauthorized | Basic Auth 未通过 | 重新访问 `/ota` 登录 |
| OTA 中断 | 网络断开、固件过大、供电不稳 | 重新上传，检查 free sketch space |
| WDT OTA 中重启 | OTA pause/resume 未生效或业务阻塞 | 查看 WDT 日志 |

---

## 十、相关文档

- 使用主线：`docs/00_user_guide.md`
- API：`docs/03_api_reference.md`
- 日志页和文件日志：`docs/07_observability.md`
- 故障排查：`docs/10_troubleshooting.md`
