# 网络与 LOCAL 能力验证结果

验证日期：2026-09-08 至 2026-09-19。范围为本库 NTP、MQTT 写入、Web 流式发送及既有 OTA/裁剪回归；不是整个平台接入完成声明。

## MQTT PINGRESP 零正文处理

真实 ESP8266 以 60 秒 keepalive 空闲运行时约每 120 秒断开。根因为固定内存解析器读完 `PINGRESP` 的 remaining-length=0 后，TLS socket 已无后续可读字节，旧循环未分发这个已完整的零正文控制包；`pingOutstanding` 因而不清除，并在下一 keepalive 截止主动关闭 TCP。修复后，解析循环会在控制包正文已完整时直接分发，不依赖 socket 还有下一字节；PUBLISH 仍要求其 Topic 和正文正常到达。

`bash tools/test_mqtt_fixed.sh` 增加 PINGRESP、未完成控制包、普通控制包和 PUBLISH 边界并通过；静态检查、Base ESP12F 构建及真实消费端构建通过。真实低压 LED 核心板只写入应用固件，保留原 WiFi/LittleFS 配置；修复前 health 为 `mqtt=backoff`、`mqttAttempt=11`、`mqttLastReason=tcp_disconnected`，修复后首次连接持续至少 226 秒，跨过三个 60 秒保活周期，始终 `mqttConnected=true`、attempt=1、reason=none、无 WDT；末段 heap 6168B、maxBlock 5688B。15 秒空闲订阅仅有 173B retained availability。该结果完成根因定向验收，但 1～2 天观察仍由用户后续执行，不把 226 秒证明外推为长稳。

## HTTP小响应收口与压力复验

修正预解析503的Content-Length（实际`busy retry`为10B，不是11B）。两个图标入口共用无临时Header String的204响应，使用已有有界/部分写处理的Flash发送器，并明确关闭连接；204不带正文或Content-Length。TLS、lwIP、Web闸门、客户端间隔和超时均未放宽。

当前HTTP候选在同一授权ESP12F完成3×180s，每轮以TLS在线开始：421/423/452个请求，全部0失败、0个503、0重启/OOM；掉线样本16/15/15，轮间及最后均自动恢复。最低health样本heap6312B/块4552B，最终heap8464B/块4552B且MQTT在线。完成请求量低于文档的约550～700参考区间，如实记录，不宣称吞吐性能提升；没有通过改变客户端节奏刷低压力。该结果满足本次错误/重启门禁，不代替长期、物理负载或每个瞬时堆谷值的测量。

静态/逻辑检查及目标构建通过，RAM51612B/Flash599135B，54067B门禁余2455B。原失败证据保留，本轮证据在设备`.artifacts/sdk-http-replies`；命令/ACK及OTA机制沿用下节已实测的同源实现，HTTP改动仅涉及图标和拒绝响应。

## 实机TLS重连OOM与内存准入

ESP12F实际接入候选在三客户端压力后，以heap13584B/连续块9632B重连，触发`Unhandled C++ exception: OOM`。匹配ELF解码为Core `make_shared<br_x509_minimal_context>`分配3080B失败；没有把异常当作正常恢复。选定Core的SSL/X509结构实际为3408/3064B；计入4096/1024缓冲、325/85协议开销及分配余量后，新建连接要求总heap16384B/最大块7168B，不足则有界等待。初版要求整个工作集连续，实测会阻塞健康碎片堆，已纠正为最大单项分配。

修正候选3轮各180s、每轮以TLS在线开始，均0重启、0OOM，并在压力后以heap16680/块9632成功重连；两轮HTTP零失败，第三轮一个icon超时，**完整Web稳定性门禁仍未通过**，不能写成整机合格。最小health样本heap5872B/块4168B；它们不等于请求内部瞬时谷值。证据在设备`.artifacts/sdk-current-5167909`、`sdk-tls-admission`、`sdk-tls-admission-v2`，失败原始证据均保留。

同时完成真实开发API的2秒命令accepted→succeeded、对应业务事实落库、同stream累计ACK3/4/5。OTA预备窗口heap17288B，最终消息PUBACK后释放TLS；预备超时自动恢复、真实HTTP OTA上传/重启通过，写入开始heap12.75KiB。重启后同stream续号6/ACK6，观察期间未重放已检查点的≤5事实。TLS/Web参数及54067B静态门禁未放宽；当前RAM51624B、Flash599067B。上述为短期指定实验板证据，非长期或物理负载验收。

## 当前记录格式与逆序读取

按试运行项目零历史兼容/零数据迁移要求，已移除暂存导入API及专用测试，保留当前格式的CRC、恢复、释放保护和逆序查询。`tools/test_record_store.sh`的原恢复/切点/维护/容量保护及300轮回归，加逆序/跨空洞/别名读取检查通过；静态、逻辑检查通过。

设备`python3 scripts/test_record_history.py`直接编译生产Base Store、SDK RecordStream及设备当前格式门面，ASAN/UBSAN验证直接初始化、4088条/127176逻辑字节、满拒绝、本地分页、ACK、检查点失败/重启、释放轮转、CRC错误不清空和维护保护。不读取旧格式，不含迁移测试。最新实际固件RAM51576B/Flash598859B，54067B静态门禁余2491B，设备36项回归通过；未烧录，不是物理FS或TLS/Web/OTA实机验收。

## 固定网络日志模板的低风险收缩

WiFi/NTP/MQTT的51处普通固定格式调用改为既有 `_P` 宏。与修改前归一化比较，除宏的存储选择外源代码完全一致，全部fmt都是字面量；没有修改格式文字、tag、参数、分支、状态或配置。当前目标ELF中 `vsnprintf_P` 尾调用同一 `vsnprintf` 实现，未新增格式缓冲或堆申请；Base日志分发/截断仍复用原实现。

`python3 tools/check_logic.py`通过，含三个模块的固定模板Flash调用检查。真实消费端 `devices/esp12f-relay` 的 `pio run -e esp12f` 通过：RAM51088B/81920B（62.363%），Flash593759B；相对相同候选改动前少3184B RAM、多68B Flash，54067B静态门禁余2979B。没有重复所有示例/芯片矩阵，没有烧录或实机验证；不能把静态通过当作完整设备可用。日志为本机 `/tmp/esp8266-log-storage-logic.log`、`/tmp/esp12f-sdk-build.log`。

## ESP8266资源准入与低内存实现

后续ESP8266接入按用户指定的ESP12F继电器实测规范，以 `docs/04_memory_budget.md` 为通用准入入口。静态门禁固定54067B；MQTT默认RX窗口已改为64B，完整入站上限仍为768B。MQTT示例采用既有实测lwIP组合7/4，未改TLS4096/1024、两出站槽、Web闸门/错峰、WDT或存储保护。

Web的PROGMEM发送缓冲改为96B；health/hostname使用私有96B分块JSON编码器，目标对象<=112B，不增加全局缓冲/JSON文档/堆。所有字符串统一转义、数值完整输出，修复合法最长hostname组合超过旧160B格式缓冲而被截断的问题。仍仅按块发送，保持整次正文30秒预算、1500ms SDK写超时和失败关连接；不把它说成完整HTTP硬截止。

- `python3 tools/test_resource_budget.py`：5组通过，包括54067/54068B精确边界、RAM口径、畸形/重复/缺失输出和失败退出码；Base测试入口及SDK8266示例已接线，增量不重链也执行门禁。
- `bash tools/test_mqtt_fixed.sh`：默认/614B MQTT、生产NTP/完整写helper及新Web JSON ASAN/UBSAN测试通过；包括301种长度、最长hostname、控制字符/UTF-8、整数极值、逐块发送失败后不续写、结束状态和空sink。主机sink不是实机TCP。
- `python3 tools/test_mqtt_prepare.py`、`python3 tools/check_logic.py`、`bash tools/check_static.sh`及脚本语法/差异检查通过；检查器旧的JSON格式化文本匹配同步更新为实际编码调用及有界块断言。
- 定向构建：根项目（LOCAL/完整Web）46400B RAM / 437056B Flash；MQTT Terminal 45160B / 515079B；SDK MQTT示例52980B / 544559B。根项目比之前46640B减少240B，SDK示例比53376B减少396B；后者是整份配置的差值，不是SDK独占成本。原生完整矩阵与其他板型未重复。
- SDK示例静态门禁报告64.673%；这不证明12槽真实产品已适配。当前无这组改动的真实TLS/Web并发、运行峰值、OTA往返或烧录证据，不能替代3×180秒及长稳验证。日志在两库本机 `.cache/resource-*`，不入包。
- 发布包88个文件均受管且与源码逐字节一致，无私密/构建/Python缓存内容；临时包已删除。

## 当前实现判断

- 继续使用本库已有的固定 MQTT 队列、收发容量分离、QoS1 确认、连接恢复、OTA 准备租约及看门狗保护。没有缩减 4096/1024 BearSSL 缓冲、恢复预算或存储预留，也没有增加任务或第二份持久 outbox。
- 主动 NTP 增加 8 字节请求关联标识，先检查等待超时再收包；DNS 耗时不计入网络 RTT。解析保留小数并处理 2036 秒字段回绕；当前 UTC 失效会关闭时间门控。请求关联不等于 NTP 身份认证。
- 主动 UDP socket 延迟到实际请求时创建；原有同步成功释放行为保留。新增常驻请求标识为 8 字节，模块估算预算由 224B 调整为 232B。
- MQTT 完整写入的超时覆盖持续部分进展。Web 补齐部分写入，整次发送 API 共用 30 秒预算；内置页面单次 SDK write 仍为 1500ms。健康 JSON 正文共用一次预算，失败关闭连接。没有新增发送大缓冲；这类协作式预算不能抢占 SDK 同步调用，也不代表整个 HTTP 请求的硬截止。

## 自动验证

`bash tools/test_all.sh` 是完整入口，包含静态/契约/原生检查、OTA上传工具及根项目、七个示例默认环境和三个裁剪/Store组合。此前完整矩阵证据保留；本次资源收缩只执行上节所列定向检查与三个相关固件构建，下方历史链接数字不能冒充全矩阵重新测量。

新增原生测试直接使用生产 NTP 时间戳解析和网络完整写入代码，覆盖请求匹配/错配、零时间戳、秒字段回绕、小数、部分写、无进展、慢进展、断连和计时回绕。静态检查另核对主动 NTP 的超时与 DNS/发送顺序；这些不是完整 UDP 或 SDK 时钟集成测试。

构建使用 PlatformIO Espressif 8266 4.2.1、Arduino Core 3.1.2、Xtensa GCC 10.3.0、ESP-12E/ESP-12F 目标。资源是链接时静态 RAM 和 Flash，不代表运行堆峰值。

| NTP/网络修正构建参考（此前测量） | RAM (B) | Flash (B) |
|---|---:|---:|
| 根项目 | 46540 | 436412 |
| basic_wifi | 34240 | 321639 |
| custom_web | 41364 | 401316 |
| full_demo | 46540 | 436412 |
| mqtt_terminal | 45372 | 514131 |
| sleep_watchdog | 34080 | 324859 |
| wifi_config_ota | 44976 | 428656 |
| mqtt_terminal / no-fs | 42668 | 477699 |
| full_demo / no-filelog | 44984 | 430808 |

未发现本次 C/C++ 修改引入的编译警告。上游 `elf2bin.py` 的 Python 无效转义 `SyntaxWarning` 仍存在，产物生成成功；未修改全局 SDK 来隐藏该警告。

## 证书及证据边界

当前 SDK 的 `x509_minimal.c` 使用系统时间检查证书有效期，已安装 `libbearssl.a` 中的 `x509_minimal.o` 保留对 `time` 的引用。本库继续要求 trust anchors、主机名验证及有效 UTC，不采用 insecure 模式。本结果不等于真实服务器证书链、过期证书或错误主机名的板端握手验证。

未执行烧录、OTA、复位或执行器动作；未验证真实弱网、多客户端 Web/TLS 并发、长期运行、掉电及 Flash 寿命。未扩展验证 NodeMCU 板型或其他 Core 版本。必须在具体设备上完成适用的受控验证后，才能声明现场可靠性。

## 平台适配边界

本库不解释平台身份、Topic、消息或业务记录确认。现已提供可选单流RecordStore和每次实际连接前准备LWT的公开入口，分别承担通用可靠存储与明确调用时机。诊断Journal不代替事实Store。独立平台SDK仍需负责协议编码、连续序号、业务ACK和补发；基础库功能完成不等于平台已接通。


## 2026-09-09 LOCAL恢复与MQTT退出复审结果

- mDNS只有启动成功才更新facade标志并发布HTTP服务；失败不沿用旧运行状态，按5秒间隔继续恢复。新增5B状态字段，未改变WiFi radio恢复阈值或增加后台任务。
- `clearAll()` 只有完整删除成功才取消deferred待写值，失败返回false并保留待写队列；这是非事务删除，已删除文件不会自动回滚，后续flush可能重新写回待写项。文件名缓冲覆盖最大key的`.tmp`/`.bak`路径；非配置文件保持。full_demo长按清理失败时恢复Watchdog并继续运行，释放按键后才允许重试，不误报恢复出厂完成。
- 正常MQTT退出在匹配PUBACK后写DISCONNECT，并检查SDK排空结果，使用整个shutdown的剩余预算，过期不传入会选择SDK默认等待的零预算；排空失败/超时返回DISCONNECT_TIMEOUT，写失败返回DISCONNECT_FAILED。单个同步SDK调用仍不能被抢占，不承诺硬截止或Broker业务处理完成。

生产Config/mDNS原生测试覆盖部分删除失败、待写重试、最大key恢复文件、成功/空目录清理、业务文件保留，以及mDNS失败限频/恢复/重新初始化失败/计时回绕。网络生产helper测试覆盖排空成功、失败、预算耗尽、SDK返回已过截止及计时回绕；契约检查确认真实MQTT退出路径调用已检查的helper。

本轮合并执行`tools/test_all.sh`通过：静态/逻辑检查、Config/mDNS生产测试、两种固定MQTT payload、NTP/网络helper、OTA上传工具及9个ESP-12F默认/裁剪构建；无FS和无FileLog符号/页面裁剪检查通过。随后最大key清理和示例失败处理的局部收尾只补跑生产配置边界、契约检查和根项目增量构建，未重复全矩阵。

最终根项目（full_demo源）RAM46,640B、Flash436,624B；与上轮同组合46,540B/436,412B相比增加100B/212B，包括恢复状态与示例错误处理，不将差值归因于单个函数。本轮首次合并构建的MQTT Terminal为45,372B/514,195B，无FS组合为42,668B/477,747B；这些数字不是运行堆峰值，也不冒充最后示例局部改动后的逐环境新测值。BearSSL 4096B RX/1024B TX、固定队列、重连和OTA保护均未缩减。

没有烧录、串口、真实Broker、WiFi中断或执行器操作。此前ESP32实验板授权不能用于其他ESP8266设备；本轮实机指标、Flash故障和长期稳定性仍未覆盖。连接前准备入口和通用可靠记录Store的实现与证据见下节；平台SDK接入仍未完成。


## 连接前准备与可靠记录存储

已实现：WiFi/可信时间/连接许可及退避门控后的单次LWT准备回调；拒绝和非法长度/Topic不发送TLS/CONNECT、不累计radio故障。回调使用固定指针，topic复用原缓冲、payload借用，不增加平台协议语义。

可选RecordStore提供固定宽度编码、16B持久世代、最多8段的直接定位读取、未释放保护、RAM释放与低频检查点；不存在不自动创建，损坏不自动清空。原生生产代码测试覆盖标准CRC向量、逐字节追加中断12种、轮转提交中断112种、CRC/头/世代错误、全部未释放时拒绝、释放后回收、检查点失败重扫、维护暂停/恢复、FS余量拒绝、256B payload及300轮追加/释放/重启循环。故障注入使用主机假文件系统，验证Base写入顺序与恢复决策，**不是真实LittleFS块故障或物理掉电证明**；提交原子性依据锁定SDK所带LittleFS的rename/sync契约。

MQTT测试执行生产prepare和handle门控方法，以假transport覆盖初次/失败重试、拒绝准备、超长payload/topic、借用变长内容和已连接时不重复准备。测试边界是TLS调用之前，不声称完整TLS/MQTT实机握手通过。OTA/重启/休眠的Store调用顺序与失败恢复有契约检查；LOCAL+Store及MQTT+Store编译覆盖集成。未向看门狗/异常复位新增Flash写入。

集中默认集成测试10个构建通过，随后仅对改动过的MQTT示例增量复测，并新增MQTT+Store组合；当前默认入口包含11个组合。未重复其他Core或NodeMCU矩阵。无新C/C++警告，上游elf2bin.py的Python SyntaxWarning保留。新增Store静态控制状态192B（编译上限256B），关闭时根项目无Store符号且RAM/Flash均不变。未压缩TLS、重连、outbox或OTA保护预算。

| 连接准备/Store组合参考（此前测量） | RAM (B) | Flash (B) |
| --- | ---: | ---: |
| 根项目，Store关闭 | 46640 | 436624 |
| record_store，LOCAL+Store | 41124 | 387899 |
| mqtt_terminal，Store关闭 | 45512 | 514703 |
| mqtt_terminal，无FS | 42808 | 478223 |
| mqtt_terminal，启用Store | 45944 | 517639 |

MQTT+Store启用相对该示例关闭Store增加432B静态RAM/2936B Flash（只使用打开、校验和维护路径，不等于全部API成本）。上述数字是链接静态占用，不是运行堆峰值。默认/无FS的Store符号裁剪、MQTT+Store符号存在均通过。

ESP8266主线的本批功能、相关验证和契约已收齐；平台SDK实现与平台/真实Broker链路属于后续平台阶段，ESP8266实验板、弱网并发、真实掉电/欠压/FS满与长期寿命仍待适用授权和实测。两个Base永久独立。

交付包已核对84个受管文件与源码内容一致，包含新Store源码、示例及docs契约；显式导出白名单修正默认漏打docs的问题，无缓存/私密/未跟踪文件。临时tarball检查后删除，未发布或推送。

SDK接续复审补充：连续轮转后写入不完整时，最后一条已释放的完整事实仍必须保留，直到更新记录成功提交。新增6次连续中断回归修前复现、修后通过；不新增持久副本或静态状态，仅轮转选择排除最新完整记录所在段。只重跑Store原生和LOCAL+Store目标增量构建（6.54秒），RAM不变、Flash增加112B，不重复其他环境矩阵；前表MQTT+Store是追加此纯Store轮转修正之前的测值。
