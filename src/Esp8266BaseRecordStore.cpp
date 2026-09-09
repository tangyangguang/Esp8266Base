#include "Esp8266BaseRecordStore.h"
#if ESP8266BASE_USE_RECORD_STORE
#include "Esp8266BaseFilesystem.h"
#include "Esp8266BaseLog.h"
#include <LittleFS.h>
#include <string.h>

namespace {
using Result = Esp8266BaseRecordStoreResult;
struct Segment { uint64_t first; uint16_t count; bool sealed; };
struct StoreState {
    Segment segments[8];
    Esp8266BaseRecordStoreConfig config;
    uint8_t generation[16];
    uint64_t released, savedRelease, nextFirst;
    bool ready, paused;
    Result result, maintenance;
};
static StoreState store = {};
static_assert(sizeof(StoreState) <= 256, "Record Store metadata budget exceeded");
constexpr size_t META_BYTES = 64, SEGMENT_BYTES = 48;
const char* const META = "/eb_records/meta";
const char* const META_TMP = "/eb_records/meta.tmp";
const char* const SEGMENT_TMP = "/eb_records/segment.tmp";

bool fail(Result result, bool fatal = false) {
    store.result = result;
    if (fatal) store.ready = false;
    return false;
}
bool success() { store.result = Result::OK; return true; }
void put(uint8_t* p, uint64_t v, size_t n) { for (size_t i=0;i<n;++i) { p[i]=v & 255; v >>= 8; } }
uint64_t get(const uint8_t* p, size_t n) {
    uint64_t v=0; for (size_t i=0;i<n;++i) v |= uint64_t(p[i]) << (8*i); return v;
}
uint32_t crc(const uint8_t* data, size_t size, uint32_t value=0xffffffffUL) {
    for (size_t i=0;i<size;++i) {
        value ^= data[i];
        for (uint8_t j=0;j<8;++j) value=(value>>1) ^ ((value&1) ? 0xedb88320UL : 0);
    }
    return value;
}
void finishHeader(uint8_t* bytes, size_t size) { put(bytes+size-4, ~crc(bytes,size-4),4); }
bool validHeader(const uint8_t* bytes, size_t size, const char* magic) {
    return !memcmp(bytes,magic,4) && get(bytes+size-4,4)==uint32_t(~crc(bytes,size-4));
}
void segmentPath(uint8_t index, char path[24]) { snprintf(path,24,"/eb_records/s%u",unsigned(index)); }
bool validConfig(const Esp8266BaseRecordStoreConfig& c) {
    return c.payloadBytes && c.payloadBytes<=256 && c.recordsPerSegment &&
        c.recordsPerSegment<=1024 && c.segmentCount>=2 && c.segmentCount<=8 && c.reserveBytes>=8192;
}
bool spaceFor(uint32_t bytes) {
    FSInfo info;
    if (!LittleFS.info(info) || info.usedBytes>info.totalBytes) return fail(Result::IO_ERROR);
    const uint64_t required=uint64_t(store.config.reserveBytes)+bytes;
    return uint64_t(info.totalBytes-info.usedBytes)>=required || fail(Result::FS_RESERVE);
}
bool exact(File& f, uint8_t* data, size_t size) { return f.read(data,size)==int(size); }
bool writeFile(const char* path, const uint8_t* bytes, size_t size) {
    File f=LittleFS.open(path,"w");
    if (!f) return false;
    const bool written=f.write(bytes,size)==size;
    f.flush(); f.close();
    if (!written) return false;
    f=LittleFS.open(path,"r");
    uint8_t verify[64];
    const bool ok=f && f.size()==size && size<=sizeof(verify) && exact(f,verify,size) && !memcmp(bytes,verify,size);
    f.close(); return ok;
}
bool saveMeta(uint64_t nextFirst) {
    if (!spaceFor(4096)) return false;
    uint8_t bytes[META_BYTES]={};
    memcpy(bytes,"EBR1",4); put(bytes+4,1,2);
    put(bytes+6,store.config.payloadBytes,2); put(bytes+8,store.config.recordsPerSegment,2);
    bytes[10]=store.config.segmentCount;
    memcpy(bytes+12,store.generation,16); put(bytes+28,store.released,8); put(bytes+36,nextFirst,8);
    finishHeader(bytes,sizeof(bytes));
    if (!writeFile(META_TMP,bytes,sizeof(bytes)) || !LittleFS.rename(META_TMP,META)) return fail(Result::IO_ERROR,true);
    store.savedRelease=store.released; store.nextFirst=nextFirst;
    return success();
}
// CRC binds each payload to its generation and physical ID, without duplicating them on disk.
uint32_t recordCrcStart(uint64_t id) {
    uint8_t encoded[8]; put(encoded,id,8);
    return crc(encoded,8,crc(store.generation,16));
}
bool verifyRecord(File& f, uint64_t id, uint8_t* payload) {
    uint8_t chunk[32], checksum[4]; uint32_t value=recordCrcStart(id);
    size_t left=store.config.payloadBytes, offset=0;
    while (left) {
        const size_t size=left<sizeof(chunk) ? left : sizeof(chunk);
        if (!exact(f,chunk,size)) return false;
        value=crc(chunk,size,value);
        if (payload) memcpy(payload+offset,chunk,size);
        left-=size; offset+=size;
    }
    return exact(f,checksum,4) && get(checksum,4)==uint32_t(~value);
}
int newestSegment() {
    int newest=-1;
    for (uint8_t i=0;i<store.config.segmentCount;++i)
        if (store.segments[i].first && (newest<0 || store.segments[i].first>store.segments[newest].first)) newest=i;
    return newest;
}
bool rotate(int& active) {
    uint64_t latestComplete=0;
    for (uint8_t i=0;i<store.config.segmentCount;++i) {
        const Segment& s=store.segments[i];
        if (s.count && s.first+s.count-1>latestComplete) latestComplete=s.first+s.count-1;
    }
    int target=-1;
    for (uint8_t i=0;i<store.config.segmentCount;++i) {
        const Segment& s=store.segments[i];
        if (!s.first) { target=i; break; }
        // Reserve the entire segment ID range, including incomplete-tail holes.
        if (s.count && (s.first+s.count-1>store.released || s.first+s.count-1==latestComplete)) continue;
        // Keep the last complete fact until a newer append commits, even across
        // repeated torn writes into otherwise empty/released segments.
        if (target<0 || s.first<store.segments[target].first) target=i;
    }
    if (target<0) return fail(Result::FULL);
    const uint64_t first=store.nextFirst;
    if (first>UINT64_MAX-store.config.recordsPerSegment) return fail(Result::ID_EXHAUSTED);
    // Persist both release protection and reserved ID range BEFORE replacing an old segment.
    if (!saveMeta(first+store.config.recordsPerSegment)) return false;
    uint8_t bytes[SEGMENT_BYTES]={};
    memcpy(bytes,"EBS1",4); put(bytes+4,1,2); put(bytes+6,target,2);
    memcpy(bytes+8,store.generation,16); put(bytes+24,first,8);
    put(bytes+32,store.config.payloadBytes,2); put(bytes+34,store.config.recordsPerSegment,2);
    finishHeader(bytes,sizeof(bytes));
    char path[24]; segmentPath(target,path);
    if (!writeFile(SEGMENT_TMP,bytes,sizeof(bytes)) || !LittleFS.rename(SEGMENT_TMP,path)) return fail(Result::IO_ERROR,true);
    store.segments[target]={first,0,false}; active=target;
    return success();
}
}

bool Esp8266BaseRecordStore::begin(const Esp8266BaseRecordStoreConfig& config) {
    if (store.paused) return fail(Result::PAUSED);
    store={};
    if (!validConfig(config)) return fail(Result::INVALID_ARGUMENT);
    store.config=config;
    if (!Esp8266BaseFilesystem::isReady()) return fail(Result::NOT_READY);
    File meta=LittleFS.open(META,"r");
    if (!meta) return fail(LittleFS.exists(META) ? Result::IO_ERROR : Result::NOT_FOUND);
    uint8_t bytes[META_BYTES];
    bool ok=meta.size()==sizeof(bytes) && exact(meta,bytes,sizeof(bytes)); meta.close();
    if (!ok || !validHeader(bytes,sizeof(bytes),"EBR1") || get(bytes+4,2)!=1) return fail(Result::CORRUPT);
    if (get(bytes+6,2)!=config.payloadBytes || get(bytes+8,2)!=config.recordsPerSegment || bytes[10]!=config.segmentCount)
        return fail(Result::CONFIG_MISMATCH);
    memcpy(store.generation,bytes+12,16); store.released=get(bytes+28,8);
    store.savedRelease=store.released; store.nextFirst=get(bytes+36,8);
    bool nonzero=false; for (uint8_t b:store.generation) nonzero |= b!=0;
    if (!nonzero || !store.nextFirst || (store.nextFirst-1)%config.recordsPerSegment || store.released>=store.nextFirst)
        return fail(Result::CORRUPT);
    for (uint8_t i=0;i<config.segmentCount;++i) {
        char path[24]; segmentPath(i,path);
        if (!LittleFS.exists(path)) continue;
        File f=LittleFS.open(path,"r");
        if (!f) return fail(Result::IO_ERROR);
        const size_t size=f.size();
        if (size<SEGMENT_BYTES || !exact(f,bytes,SEGMENT_BYTES) || !validHeader(bytes,SEGMENT_BYTES,"EBS1") ||
            get(bytes+4,2)!=1 || get(bytes+6,2)!=i || memcmp(bytes+8,store.generation,16) ||
            get(bytes+32,2)!=config.payloadBytes || get(bytes+34,2)!=config.recordsPerSegment) return fail(Result::CORRUPT);
        Segment& segment=store.segments[i]; segment.first=get(bytes+24,8);
        const size_t recordBytes=config.payloadBytes+4;
        const size_t count=(size-SEGMENT_BYTES)/recordBytes;
        if (!segment.first || (segment.first-1)%config.recordsPerSegment ||
            segment.first>=store.nextFirst || store.nextFirst-segment.first<config.recordsPerSegment ||
            count>config.recordsPerSegment || size>SEGMENT_BYTES+recordBytes*config.recordsPerSegment) return fail(Result::CORRUPT);
        for (uint8_t j=0;j<i;++j) if (store.segments[j].first==segment.first) return fail(Result::CORRUPT);
        segment.count=count; segment.sealed=(size-SEGMENT_BYTES)%recordBytes!=0;
        for (uint16_t j=0;j<segment.count;++j) {
            if (!verifyRecord(f,segment.first+j,nullptr)) return fail(Result::CORRUPT);
            yield();
        }
    }
    store.ready=true; return success();
}

bool Esp8266BaseRecordStore::rebuild(const Esp8266BaseRecordStoreConfig& config, const uint8_t generation[16]) {
    if (store.paused) return fail(Result::PAUSED);
    bool nonzero=false; if (generation) for (uint8_t i=0;i<16;++i) nonzero |= generation[i]!=0;
    if (!validConfig(config) || !nonzero) return fail(Result::INVALID_ARGUMENT);
    if (!Esp8266BaseFilesystem::isReady()) return fail(Result::NOT_READY);
    File existing=LittleFS.open(META,"r");
    if (existing) {
        uint8_t previous[META_BYTES];
        const bool same=existing.size()==sizeof(previous) && exact(existing,previous,sizeof(previous)) &&
            validHeader(previous,sizeof(previous),"EBR1") && !memcmp(previous+12,generation,16);
        existing.close();
        if (same) return fail(Result::INVALID_ARGUMENT);
    }
    // Copy before state reset.
    uint8_t newGeneration[16]; memcpy(newGeneration,generation,16);
    store={}; store.config=config; memcpy(store.generation,newGeneration,16);
    if (!LittleFS.exists("/eb_records") && !LittleFS.mkdir("/eb_records")) return fail(Result::IO_ERROR);
    // Remove metadata first. Interrupted rebuild can never appear as the old valid store.
    const char* fixed[]={META,META_TMP,SEGMENT_TMP};
    for (const char* path:fixed) if (LittleFS.exists(path) && !LittleFS.remove(path)) return fail(Result::IO_ERROR);
    for (uint8_t i=0;i<8;++i) {
        char path[24]; segmentPath(i,path);
        if (LittleFS.exists(path) && !LittleFS.remove(path)) return fail(Result::IO_ERROR);
    }
    if (!saveMeta(1)) return false;
    store.ready=true; return success();
}
bool Esp8266BaseRecordStore::append(const uint8_t* payload, size_t length, uint64_t& id) {
    id=0;
    if (store.paused) return fail(Result::PAUSED);
    if (!store.ready) return fail(Result::NOT_READY);
    if (!payload || length!=store.config.payloadBytes) return fail(Result::INVALID_ARGUMENT);
    if (!spaceFor(uint32_t(length)+4096)) return false;
    int active=newestSegment();
    if (active<0 || store.segments[active].sealed || store.segments[active].count==store.config.recordsPerSegment)
        if (!rotate(active)) return false;
    Segment& segment=store.segments[active];
    const uint64_t candidate=segment.first+segment.count;
    char path[24]; segmentPath(active,path);
    File f=LittleFS.open(path,"a");
    const size_t offset=SEGMENT_BYTES+size_t(segment.count)*(length+4);
    if (!f || f.size()!=offset) return fail(Result::IO_ERROR,true);
    uint8_t checksum[4]; put(checksum,uint32_t(~crc(payload,length,recordCrcStart(candidate))),4);
    const bool written=f.write(payload,length)==length && f.write(checksum,4)==4;
    f.flush(); f.close();
    if (!written) return fail(Result::IO_ERROR,true);
    f=LittleFS.open(path,"r");
    if (!f || f.size()!=offset+length+4 || !f.seek(offset,SeekSet) || !verifyRecord(f,candidate,nullptr)) return fail(Result::IO_ERROR,true);
    ++segment.count; id=candidate; return success();
}
bool Esp8266BaseRecordStore::readById(uint64_t id, uint8_t* payload, size_t capacity) {
    if (store.paused) return fail(Result::PAUSED);
    if (!store.ready) return fail(Result::NOT_READY);
    if (!payload || capacity<store.config.payloadBytes || !id) return fail(Result::INVALID_ARGUMENT);
    for (uint8_t i=0;i<store.config.segmentCount;++i) {
        const Segment& s=store.segments[i];
        if (!s.first || id<s.first || id-s.first>=s.count) continue;
        char path[24]; segmentPath(i,path); File f=LittleFS.open(path,"r");
        const size_t offset=SEGMENT_BYTES+size_t(id-s.first)*(store.config.payloadBytes+4);
        if (!f || !f.seek(offset,SeekSet)) return fail(Result::IO_ERROR,true);
        if (!verifyRecord(f,id,payload)) return fail(Result::CORRUPT,true);
        return success();
    }
    return fail(Result::NOT_FOUND);
}
bool Esp8266BaseRecordStore::readNext(uint64_t afterId, uint64_t& id, uint8_t* payload, size_t capacity) {
    id=0;
    if (store.paused) return fail(Result::PAUSED);
    if (!store.ready) return fail(Result::NOT_READY);
    if (!payload || capacity<store.config.payloadBytes) return fail(Result::INVALID_ARGUMENT);
    uint64_t next=0;
    for (uint8_t i=0;i<store.config.segmentCount;++i) {
        const Segment& s=store.segments[i];
        if (!s.count || afterId>=s.first+s.count-1) continue;
        const uint64_t candidate=afterId<s.first ? s.first : afterId+1;
        if (!next || candidate<next) next=candidate;
    }
    if (!next) return fail(Result::NOT_FOUND);
    if (!readById(next,payload,capacity)) return false;
    id=next; return true;
}
bool Esp8266BaseRecordStore::readPrevious(uint64_t beforeId, uint64_t& id,
                                         uint8_t* payload, size_t capacity) {
    id=0;
    if (store.paused) return fail(Result::PAUSED);
    if (!store.ready) return fail(Result::NOT_READY);
    if (!payload || capacity<store.config.payloadBytes) return fail(Result::INVALID_ARGUMENT);
    uint64_t previous=0;
    for (uint8_t i=0;i<store.config.segmentCount;++i) {
        const Segment& s=store.segments[i];
        if (!s.count || beforeId<=s.first) continue;
        const uint64_t last=s.first+s.count-1;
        const uint64_t candidate=beforeId<=last ? beforeId-1 : last;
        if (candidate>previous) previous=candidate;
    }
    if (!previous) return fail(Result::NOT_FOUND);
    if (!readById(previous,payload,capacity)) return false;
    id=previous; return success();
}
bool Esp8266BaseRecordStore::releaseThrough(uint64_t id) {
    if (store.paused) return fail(Result::PAUSED);
    if (!store.ready) return fail(Result::NOT_READY);
    uint64_t latest=store.released;
    for (uint8_t i=0;i<store.config.segmentCount;++i) {
        const Segment& s=store.segments[i];
        if (s.count && s.first+s.count-1>latest) latest=s.first+s.count-1;
    }
    if (id>latest) return fail(Result::INVALID_ARGUMENT);
    if (id>store.released) store.released=id;
    return success();
}
bool Esp8266BaseRecordStore::checkpoint() {
    if (store.paused) return fail(Result::PAUSED);
    if (!store.ready) return fail(Result::NOT_READY);
    return store.released==store.savedRelease ? success() : saveMeta(store.nextFirst);
}
void Esp8266BaseRecordStore::prepareMaintenance() {
    if (store.paused) return;
    store.maintenance=Result::OK;
    if (store.ready && !checkpoint()) {
        store.maintenance=store.result;
        ESP8266BASE_LOG_E("Store","checkpoint_failed result=%u action=continue_maintenance",unsigned(store.result));
    } else if (!store.ready) store.maintenance=store.result;
    store.paused=true;
}
void Esp8266BaseRecordStore::resumeAfterMaintenance() { store.paused=false; }
bool Esp8266BaseRecordStore::isReady() { return store.ready; }
bool Esp8266BaseRecordStore::isPaused() { return store.paused; }
Esp8266BaseRecordStoreResult Esp8266BaseRecordStore::lastResult() { return store.result; }
Esp8266BaseRecordStoreResult Esp8266BaseRecordStore::maintenanceResult() { return store.maintenance; }
uint64_t Esp8266BaseRecordStore::releasedThrough() { return store.released; }
bool Esp8266BaseRecordStore::copyGeneration(uint8_t output[16]) {
    if (!output || !store.ready) return false;
    memcpy(output,store.generation,16); return true;
}
#endif
