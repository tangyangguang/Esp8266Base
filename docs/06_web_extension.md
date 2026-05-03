# Esp8266Base Web 扩展开发指南

> 版本：1.0.0  
> 模块：`Esp8266BaseWeb`  
> 文件：`Esp8266BaseWeb.h / .cpp`

---

## 一、扩展能力上限

ESP8266 Web 活跃时可用 RAM 约 18-24KB，扩展能力固定上限：

| 类型 | 上限 | 编译宏 |
|------|------|----|
| 应用自定义页面 | **4** 个 | `ESP8266BASE_WEB_MAX_APP_PAGES` |
| 应用自定义 API | **6** 个 | `ESP8266BASE_WEB_MAX_APP_APIS` |
| 路径最大长度 | **24** 字符 | 硬编码 |

每条路由占用约 32B 静态 RAM（路径 24B + 函数指针 4B + 标志 1B + padding 3B）。  
超过上限说明项目已超出 ESP8266 极简基础库定位，应考虑换 ESP32。

---

## 二、注册自定义路由

必须在 `Esp8266Base::begin()` 之后注册：

```cpp
void setup() {
    Esp8266Base::setFirmwareInfo("fan-ctrl", "1.0.0");
    Esp8266Base::setHostname("esp-fan");
    Esp8266Base::begin();

    Esp8266BaseWeb::addPage("/fan",      handleFanPage);  // GET 页面
    Esp8266BaseWeb::addApi("/api/fan",   handleFanApi);   // GET + POST
}
```

`addPage` 注册 GET 路由。`addApi` 同时响应 GET 和 POST，在 handler 内通过 `server().method()` 区分。

---

## 三、Handler 函数规范

```cpp
void handlerName();  // 无参数，无返回值
```

可以是普通函数，也可以是无捕获的 lambda：

```cpp
// 普通函数（推荐）
void handleFanPage() { ... }
Esp8266BaseWeb::addPage("/fan", handleFanPage);

// 无捕获 lambda（可转为函数指针）
Esp8266BaseWeb::addPage("/fan", []() { ... });

// 有捕获 lambda（编译错误，不支持）
int speed = 100;
Esp8266BaseWeb::addPage("/fan", [speed]() { ... });
```

访问请求参数和方法：

```cpp
void handleFanApi() {
    if (!Esp8266BaseWeb::checkAuth()) return;

    if (Esp8266BaseWeb::server().method() == HTTP_POST) {
        int speed = Esp8266BaseWeb::server().arg("speed").toInt();
        setFanSpeed(speed);
        Esp8266BaseWeb::server().send(200, "application/json", "{\"ok\":true}");
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "{\"speed\":%d}", getFanSpeed());
        Esp8266BaseWeb::server().send(200, "application/json", buf);
    }
}
```

---

## 四、页面输出辅助函数

标准页面结构：

```cpp
void handleFanPage() {
    if (!Esp8266BaseWeb::checkAuth()) return;

    Esp8266BaseWeb::sendHeader();  // HTML head + 导航栏

    // 静态部分放 PROGMEM
    static const char FAN_FORM[] PROGMEM =
        "<h2>Fan Control</h2>"
        "<form method='post' action='/api/fan' onsubmit='return once(this)'>"
        "Speed: <input name='speed' type='number' min='0' max='100'>"
        "<input type='submit' value='Set'>"
        "</form>";
    Esp8266BaseWeb::sendContent_P(FAN_FORM);

    // 动态部分用 snprintf + sendChunk
    char buf[48];
    snprintf(buf, sizeof(buf), "<p>Current: %d%%</p>", getFanSpeed());
    Esp8266BaseWeb::sendChunk(buf);

    Esp8266BaseWeb::sendFooter();  // free heap + </body></html>
}
```

### 辅助函数说明

| 函数 | 说明 |
|------|------|
| `sendHeader()` | HTTP 200 + HTML head + 内联 CSS + 导航栏（PROGMEM）|
| `sendFooter()` | free heap 显示 + `</body></html>` |
| `sendContent_P(PGM_P)` | 单遍从 PROGMEM 读取并流式输出，无二次遍历 |
| `sendChunk(const char*)` | 流式输出动态内容块（建议 <= 512B） |

`sendHeader()` 内置了轻量 `once(form)` JS 辅助函数。页面表单建议在 `onsubmit` 中调用它，避免用户连续点击提交按钮。表单 POST 完成后建议使用 `303 See Other` 重定向回 GET 页面，避免浏览器刷新时重复提交。

---

## 五、RAM 安全规则

**1. Handler 内临时缓冲不超过 64B**

```cpp
// 推荐
char buf[64];
snprintf(buf, sizeof(buf), "<p>Temp: %.1f</p>", temp);
Esp8266BaseWeb::sendChunk(buf);
```

**2. 不拼接整页 String**

```cpp
// 危险：可能消耗大量堆 RAM
String html = "<html><body>";
html += "<h1>Data</h1>";
server.send(200, "text/html", html);  // 不推荐

// 推荐：分段发送
Esp8266BaseWeb::sendHeader();
Esp8266BaseWeb::sendContent_P(PSTR("<h1>Data</h1>"));
Esp8266BaseWeb::sendFooter();
```

**3. 静态 HTML 放 PROGMEM**

```cpp
// 推荐：不占 DRAM
static const char PAGE[] PROGMEM = "<form>...</form>";

// 错误：占 DRAM
static const char PAGE[] = "<form>...</form>";
```

**4. JSON 响应保持简短**

```cpp
char buf[128];
snprintf(buf, sizeof(buf),
    "{\"speed\":%d,\"temp\":%.1f,\"heap\":%u}",
    getFanSpeed(), getTemp(), (unsigned)ESP.getFreeHeap());
Esp8266BaseWeb::server().send(200, "application/json", buf);
```

---

## 六、认证处理

所有自定义 handler 在函数开头调用认证检查：

```cpp
void handleFanPage() {
    if (!Esp8266BaseWeb::checkAuth()) return;  // 未通过则自动发 401
    // 正常处理
}
```

公开接口（如健康检查）可不调用 `checkAuth()`。

`verifyAuth()` 仅验证不发 401，供需要在接收数据前检测认证的场景使用。内置 OTA 上传 POST 不做额外认证，认证只发生在 `/ota` 页面 GET。

---

## 七、完整示例

```cpp
#include "Esp8266Base.h"

static const char FAN_PAGE[] PROGMEM =
    "<h2>Fan Control</h2>"
    "<form method='post' action='/api/fan' onsubmit='return once(this)'>"
    "Speed (0-100): <input name='spd' type='number' min='0' max='100'>"
    "<input type='submit' value='Apply'>"
    "</form>";

void handleFanPage() {
    if (!Esp8266BaseWeb::checkAuth()) return;
    Esp8266BaseWeb::sendHeader();
    Esp8266BaseWeb::sendContent_P(FAN_PAGE);
    char buf[40];
    snprintf(buf, sizeof(buf), "<p>Current: %d%%</p>", getFanSpeed());
    Esp8266BaseWeb::sendChunk(buf);
    Esp8266BaseWeb::sendFooter();
}

void handleFanApi() {
    if (!Esp8266BaseWeb::checkAuth()) return;
    if (Esp8266BaseWeb::server().method() == HTTP_POST) {
        int spd = Esp8266BaseWeb::server().arg("spd").toInt();
        setFanSpeed(constrain(spd, 0, 100));
        Esp8266BaseWeb::server().sendHeader("Location", "/fan?saved=1");
        Esp8266BaseWeb::server().send(303);
    } else {
        char buf[24];
        snprintf(buf, sizeof(buf), "{\"speed\":%d}", getFanSpeed());
        Esp8266BaseWeb::server().send(200, "application/json", buf);
    }
}

void setup() {
    Serial.begin(115200);
    Esp8266Base::setFirmwareInfo("fan-ctrl", "1.0.0");
    Esp8266Base::setHostname("esp-fan");
    Esp8266Base::begin();

    Esp8266BaseWeb::addPage("/fan",     handleFanPage);
    Esp8266BaseWeb::addApi("/api/fan",  handleFanApi);
}

void loop() {
    Esp8266Base::handle();
}
```

---

## 八、不支持的用法

| 用法 | 替代方案 |
|------|----------|
| 超过 4 个页面 / 6 个 API | 合并页面，或换 ESP32 |
| `std::function` handler | 普通函数或无捕获 lambda |
| 有捕获 lambda | 改用全局变量 + 普通函数 |
| 动态路由（通配符） | 在单个 handler 内自行解析路径参数 |
| 页面模板引擎 | 手动 `sendContent_P` + `sendChunk` |
| 大 JSON builder（ArduinoJson 等） | 手动 `snprintf` 拼 JSON |
| HTTPS | 不支持（ESP8266 RAM 不足） |
| WebSocket | 不支持 |
