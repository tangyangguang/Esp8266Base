# 通用可靠记录存储

`Esp8266BaseRecordStore` 是 ESP8266 独立实现的单流、固定宽度、可裁剪事实日志。它只依赖 Filesystem/LittleFS 和日志，不识别设备类型、Topic、平台身份、业务 ACK 或补发协议。诊断 Journal 继续负责诊断档案；不要把同一事实再写入第二份持久 outbox。

## 启用与生命周期

构建定义 `ESP8266BASE_USE_RECORD_STORE=1`，要求 Filesystem=1，不要求 Config、FileLog、Web 或 MQTT。默认关闭，关闭时无状态、代码或初始化。`Esp8266Base::begin()` 挂载 FS 后，应用显式调用 `Esp8266BaseRecordStore::begin(config)`；库不自动创建、迁移、格式化或更换存储世代。

配置为 `Esp8266BaseRecordStoreConfig{payloadBytes, recordsPerSegment, segmentCount, reserveBytes}`：payload 1..256B，每段 1..1024 条，2..8 段，FS 额外安全余量至少 8192B。前三项是持久格式参数，重启时不同返回 CONFIG_MISMATCH。余量可以调整；逻辑容量不代表提前预留了 Flash，其他模块占满 FS 时追加仍会被拒绝。

全部方法在同一 loop 上下文调用，非重入；不得从 ISR、定时器或并发任务调用。读写缓冲由调用者持有，API 返回后不借用。不得直接把 C++ 对象、指针、String 或编译器填充写入 payload；应用明确编码版本、定宽字段、字节序。

当前试运行方案零历史兼容、零数据迁移：本库不提供旧格式导入、暂存切换或迁移API。当前格式的恢复、CRC和未释放保护不因此削弱。

## API 和错误语义

| 方法 | 契约 |
| --- | --- |
| `begin(config)` | 扫描头和全部完整记录的 CRC，成功才可读写；不存在返回 NOT_FOUND；不隐式重建。维护暂停时拒绝。 |
| `rebuild(config, generation[16])` | **显式破坏性操作**，仅删除下述本模块文件；先移除元数据，再删除段，最后提交新元数据。世代必须非零，不能等于当前有效元数据中的世代。应用负责全局唯一性；失败后不得继续把旧流视为有效。 |
| `append(payload, length, id)` | length 必须等于固定宽度；成功才返回非零物理 ID；失败 id=0。校验写入长度、关闭重开后读回 CRC。IO_ERROR 后写入结果可能不确定，必须重新 begin 并与上层事实核对；不能盲目重发追加。 |
| `readById(id, buffer, capacity)` | 只按最多 8 个段元数据定位，再直接 seek 固定槽，校验 CRC；缓冲至少 payloadBytes。失败时缓冲可能已部分修改，不可使用。 |
| `readNext(afterId, id, buffer, capacity)` | 返回最小的物理 ID > afterId，自动跨空洞；没有数据返回 NOT_FOUND、id=0。包括已释放但尚未回收的数据。 |
| `readPrevious(beforeId, id, buffer, capacity)` | 返回最大物理ID < beforeId，跨空洞；从UINT64_MAX开始逆序读取。最多检查8段元数据，不增加页面缓存。 |
| `releaseThrough(id)` | 调用者保证所有不大于 id 的事实已获业务确认；只推进 RAM 水位，不写 Flash；不得大于已知最新记录，重复/较小值幂等。 |
| `checkpoint()` | 仅释放水位变化才写元数据；轮转前总会保存水位和下段 ID 预留。无变化不写。 |
| `prepareMaintenance()` | 先尝试检查点，再暂停 Store 的读写、begin/rebuild/release/checkpoint。失败记录在 maintenanceResult，仍允许安全恢复动作。重复调用幂等。 |
| `resumeAfterMaintenance()` | 恢复访问许可；不清除 IO/CORRUPT 导致的不可用状态，必要时仍需 begin。 |
| `isReady/isPaused/lastResult/maintenanceResult/releasedThrough/copyGeneration` | 查询状态；`copyGeneration` 要求可用和非空16B输出缓冲，复制世代。 |

`FULL` 表示没有可回收段；`FS_RESERVE` 表示 FS 剩余容量不足；两者保留已有数据、可在资源条件改善后重试。`IO_ERROR/CORRUPT` 的追加/读回/元数据写入故障会停止使用当前状态，需重新扫描；重扫仍 CORRUPT 时由应用诊断和决定恢复，不能自动清空。`ID_EXHAUSTED` 拒绝分配新段，不回绕 ID。`lastResult` 是最近操作结果，维护失败另有独立查询，避免后续 PAUSED 覆盖证据。

## 文件格式与资源

专属路径为 `/eb_records/meta`、`meta.tmp`、`segment.tmp`、`s0`..`s7`。重建只触及这些文件，不删除其他业务文件、配置或 Journal。临时文件不是正式数据，begin 忽略它们，下次相应写入覆盖；不存在正式元数据时不会自动把临时文件提升为有效流。

所有整数小端，保留字节为零；CRC-32 使用反射多项式 0xEDB88320，初值/最终异或均为 0xFFFFFFFF。

| 编码 | 字节布局 |
| --- | --- |
| 元数据 64B | 0..3 `EBR1`；4..5 版本1；6..7 payload宽度；8..9 每段条数；10 段数；11 保留；12..27 世代16B；28..35 已持久释放水位；36..43 下次段首ID；44..59 保留；60..63 前60B的CRC |
| 段头 48B | 0..3 `EBS1`；4..5 版本1；6..7 槽索引；8..23 世代；24..31 首条物理ID；32..33 payload宽度；34..35 每段条数；36..43 保留；44..47 前44B的CRC |
| 记录 payloadBytes+4B | 固定payload紧随4B CRC；CRC覆盖“世代16B + 本条物理ID小端8B + payload”，不把可由段/槽推导的ID再存一份 |

ID 从1开始，每次创建新段预留一整段区间；正常重启/OTA不改变世代，物理ID允许空洞。元数据与段头通过“临时文件写入/关闭/读回 → LittleFS rename替换”提交；不先删除旧目标。轮转顺序为先持久化释放水位和预留 ID，再提交新段头覆盖已释放旧段。不能覆盖任何完整的未释放记录；即使已经释放，也保留最新完整记录所在段，直到更新的完整记录成功提交。这样连续中断/封段不会回收上层恢复连续序号所需的最后事实。段满/有不完整尾部时换段；部分释放但仍有未释放记录的段不能回收。

重启时完整记录逐条验 CRC；不完整的最后一条不会当作成功记录，保留已有有效前缀并封闭该段，不截断后继续复用槽位。完整长度但CRC不正确、头损坏、重复/越界段区间、世代或配置不匹配均拒绝使用。元数据提交后、段提交前断电会造成 ID 空洞，不能造成未确认数据被提前回收。

逻辑文件上限为 `64 + segmentCount × (48 + recordsPerSegment × (payloadBytes + 4))`，还需要临时文件、LittleFS 块分配/COW/元数据空间；追加至少检查 `reserveBytes + payloadBytes + 4096` 的可用空间，元数据提交检查 `reserveBytes + 4096`。这不是 FS 原子空间预留，实际写失败仍必须处理。只有正常轮转和应用要求的检查点写入元数据，不逐条保存 ACK 文件。

静态控制状态由编译期 `sizeof(StoreState) <= 256` 保证；没有常驻 payload 缓存，局部处理块最大64B，逐段重扫会 yield。每次 append 包含关闭重开读回，可靠性优先于省掉验证 I/O；高频业务应先评估实际写入频率和Flash寿命。Arduino LittleFS 的 flush/close 不返回底层同步错误，本库使用长度和重开读回检查，但它们不能替代真实掉电/欠压/Flash故障测试。

## MQTT、维护和平台接入

平台连续序号与事实一起编码到同一个 payload；物理ID只用于查找/释放，不能当平台连续序号。SDK解释平台 ACK、重放和中断后的事实判重，MQTT PUBACK 只表示传输确认，不足以释放持久事实。恢复后可以从持久释放水位继续顺序读取，上层必须幂等处理释放水位尚未落盘导致的重放。

启用后，库的 Web 正常重启、deepSleep、OTA准备自动做维护检查点；OTA业务准备回调成功后、MQTT暂停和 Update.begin 前暂停Store。准备失败、租约过期、上传失败均恢复Store访问；成功OTA保持暂停直至重启。维护检查点失败不会阻断恢复OTA/安全重启，但可以单独查询/记录。应用直接调用 ESP.restart/system_restart 时须自行先调用 prepareMaintenance。看门狗/异常复位路径不新增Flash写入，恢复后接受幂等重放。

`examples/record_store` 同时启用 LOCAL Web/OTA 和 Store。它不会自动创建Store：操作者明确输入串口 `I` 才清理专属Store并生成新世代，`A/N/R/C` 分别演示追加、读取、释放、检查点。实际设备不应直接照搬示例的“读过即人工释放”规则。
