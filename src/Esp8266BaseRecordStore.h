#pragma once
#include "Esp8266BaseOptions.h"
#if ESP8266BASE_USE_RECORD_STORE
#include <Arduino.h>

// 固定编码字节；单流、单 loop，所有 API 非重入。begin 不自动创建/清空。
struct Esp8266BaseRecordStoreConfig {
    uint16_t payloadBytes;       // 1..256，持久化后不可变
    uint16_t recordsPerSegment;  // 1..1024，持久化后不可变
    uint8_t segmentCount;        // 2..8，持久化后不可变
    uint32_t reserveBytes;       // 至少 8192，额外 FS 安全余量
};
enum class Esp8266BaseRecordStoreResult : uint8_t {
    OK, NOT_READY, NOT_FOUND, INVALID_ARGUMENT, CONFIG_MISMATCH,
    CORRUPT, IO_ERROR, FULL, FS_RESERVE, PAUSED, ID_EXHAUSTED
};
class Esp8266BaseRecordStore {
public:
    static bool begin(const Esp8266BaseRecordStoreConfig& config);
    // 唯一破坏性入口：清除 /eb_records 下本模块文件；新世代由调用方提供，非零16B。
    static bool rebuild(const Esp8266BaseRecordStoreConfig& config, const uint8_t generation[16]);
    // 只复制本次固定宽度 payload。失败后 id=0；IO/CORRUPT 必须 begin 重扫后再写。
    static bool append(const uint8_t* payload, size_t length, uint64_t& id);
    static bool readById(uint64_t id, uint8_t* payload, size_t capacity);
    // 返回最小的 id > afterId，含已释放但尚未回收的记录；无记录返回 NOT_FOUND。
    static bool readNext(uint64_t afterId, uint64_t& id, uint8_t* payload, size_t capacity);
    // 最大的id < beforeId，自动跨空洞；从UINT64_MAX开始可逆序查询。
    static bool readPrevious(uint64_t beforeId, uint64_t& id, uint8_t* payload, size_t capacity);
    static bool releaseThrough(uint64_t id); // RAM 水位；调用者保证此前事实已被确认
    static bool checkpoint();               // 无变化不写；轮转前自动保存
    static void prepareMaintenance();       // 检查点失败可诊断，但仍暂停写入以允许恢复动作
    static void resumeAfterMaintenance();
    static bool isReady();
    static bool isPaused();
    static Esp8266BaseRecordStoreResult lastResult();
    static Esp8266BaseRecordStoreResult maintenanceResult();
    static uint64_t releasedThrough();
    static bool copyGeneration(uint8_t output[16]);
};
#endif
