#include <cassert>
#include <cstdio>
#include "../src/Esp8266BaseRecordStore.cpp"
std::map<std::string,std::vector<uint8_t>> disk;
std::string denyRename, denyRemove;
int writeBudget=-1;
unsigned writes=0;
size_t availableBytes=1024*1024;
FakeLittleFS LittleFS;
void yield() {}
uint32_t millis() { return 0; }
bool Esp8266BaseFilesystem::isReady() { return true; }
void Esp8266BaseLog::log(uint8_t,const char*,const char*,...) {}
void Esp8266BaseLog::log_P(uint8_t,const char*,PGM_P,...) {}
using Store=Esp8266BaseRecordStore;
using R=Esp8266BaseRecordStoreResult;
Esp8266BaseRecordStoreConfig config={8,2,2,8192};
uint8_t generation[16]={1}, payload[8]={42}, output[256];
void reset() {
    Store::resumeAfterMaintenance(); disk.clear(); denyRename.clear(); denyRemove.clear(); writeBudget=-1;
    availableBytes=1024*1024; assert(Store::rebuild(config,generation));
}
void testImport() {
    const std::vector<uint8_t> source={7,8,9};
    const auto fresh=[&]() {
        Store::resumeAfterMaintenance(); disk.clear(); denyRename.clear(); denyRemove.clear();
        writeBudget=-1; availableBytes=1024*1024;
        assert(!Store::begin(config));
        disk["/history/h0.bin"]=source;
    };
    fresh(); uint64_t id=99;
    assert(!Store::beginImport(config,generation,2)); // Segment ranges stay aligned to 1.
    assert(!Store::beginImport(config,generation,UINT64_MAX));
    assert(Store::beginImport(config,generation,3));
    assert(!Store::isReady() && Store::releasedThrough()==0);
    uint8_t copied[16]; assert(!Store::copyGeneration(copied));
    assert(!Store::append(payload,8,id) && id==0);
    assert(!Store::checkpoint() && !Store::releaseThrough(0));
    assert(!Store::readNext(0,id,output,sizeof(output)) && id==0);
    assert(Store::appendImport(payload,8,id) && id==3);
    assert(!Store::commitImport(2,2));
    assert(!Store::commitImport(1,1));
    assert(!Store::commitImport(1,4));
    assert(!disk.count("/eb_records/meta"));
    assert(!Store::begin(config)); // Reboot cannot activate a partial import.
    auto before=disk;
    uint8_t other[16]={99};
    assert(!Store::beginImport(config,other,3) && disk==before);
    assert(!Store::rebuild(config,generation) && disk==before);
    assert(Store::beginImport(config,generation,3)); // Same generation, inactive copy only.
    assert(Store::appendImport(payload,8,id) && id==3);
    assert(Store::appendImport(payload,8,id) && id==4);
    assert(Store::commitImport(2,3));
    assert(Store::isReady() && Store::releasedThrough()==3);
    assert(Store::begin(config));
    assert(Store::copyGeneration(copied) && !memcmp(copied,generation,16));
    assert(Store::readNext(0,id,output,sizeof(output)) && id==3);
    assert(Store::readNext(id,id,output,sizeof(output)) && id==4);
    assert(Store::readPrevious(UINT64_MAX,id,output,sizeof(output)) && id==4);
    assert(Store::readPrevious(id,id,output,sizeof(output)) && id==3);
    assert(!Store::readPrevious(id,id,output,sizeof(output)) && id==0 && Store::lastResult()==R::NOT_FOUND);
    assert(!Store::readPrevious(0,id,output,sizeof(output)) && id==0);
    before=disk;
    assert(!Store::beginImport(config,generation,3) && disk==before);
    assert(!Store::beginImport(config,other,3) && disk==before);
    assert(!Store::appendImport(payload,8,id) && id==0);
    assert(disk["/history/h0.bin"]==source);

    fresh(); disk["/eb_records/s0"]={1,2,3}; before=disk;
    assert(!Store::beginImport(config,generation) && disk==before); // Unowned orphan: preserve.
    fresh(); disk["/eb_records/meta"]={1,2,3}; before=disk;
    assert(!Store::beginImport(config,generation) && disk==before); // Corrupt active: preserve.
    fresh(); disk["/eb_records/import"]={1,2,3}; before=disk;
    assert(!Store::beginImport(config,generation) && disk==before);

    fresh(); writeBudget=63;
    assert(!Store::beginImport(config,generation,3));
    assert(!Store::begin(config)); writeBudget=-1;
    assert(Store::beginImport(config,generation,3)); // Torn initial marker had no segments.
    writeBudget=115; // Staging meta64 + segment48 + partial payload3.
    assert(!Store::appendImport(payload,8,id) && !Store::isReady());
    writeBudget=-1; assert(!Store::commitImport(0,2));
    assert(!Store::begin(config));
    assert(Store::beginImport(config,generation,3));
    assert(Store::appendImport(payload,8,id) && id==3);
    disk["/eb_records/s0"][48]^=1;
    assert(!Store::commitImport(1,2) && Store::lastResult()==R::CORRUPT);
    assert(!disk.count("/eb_records/meta"));
    assert(Store::beginImport(config,generation,3));
    assert(Store::appendImport(payload,8,id));
    denyRename="/eb_records/meta";
    assert(!Store::commitImport(1,2) && !Store::isReady());
    assert(!Store::begin(config)); denyRename.clear();
    assert(Store::beginImport(config,generation,3));
    assert(Store::appendImport(payload,8,id) && id==3);
    denyRemove="/eb_records/import";
    assert(Store::commitImport(1,2)); // Cleanup failure is not a failed activation.
    assert(disk.count("/eb_records/import"));
    assert(Store::begin(config) && Store::releasedThrough()==2);
    before=disk;
    assert(!Store::beginImport(config,generation,3) && disk==before);
    assert(disk["/history/h0.bin"]==source);
    denyRemove.clear();

    fresh(); assert(Store::beginImport(config,generation,3));
    for (uint64_t n=3;n<=6;++n) assert(Store::appendImport(payload,8,id) && id==n);
    assert(!Store::appendImport(payload,8,id) && Store::lastResult()==R::FULL);
    assert(Store::commitImport(4,2) && Store::begin(config));
    assert(Store::readById(3,output,sizeof(output)) && Store::readById(6,output,sizeof(output)));
    fresh(); assert(Store::beginImport(config,generation));
    Store::prepareMaintenance();
    assert(!Store::appendImport(payload,8,id) && Store::lastResult()==R::PAUSED);
    assert(!Store::commitImport(0,0) && Store::lastResult()==R::PAUSED);
    Store::resumeAfterMaintenance();
    assert(Store::commitImport(0,0) && Store::begin(config));
    assert(disk["/history/h0.bin"]==source);
    puts("Import staging, retained prefix, retry, corruption, atomic activation and source preservation passed");
}
int main() {
    assert(uint32_t(~crc(reinterpret_cast<const uint8_t*>("123456789"),9))==0xcbf43926UL);
    assert(!Store::begin(config) && Store::lastResult()==R::NOT_FOUND);
    reset(); uint64_t id=0;
    disk["/app/data"]={1,2,3};
    assert(!Store::rebuild(config,generation) && Store::lastResult()==R::INVALID_ARGUMENT);
    for (uint64_t i=1;i<=4;++i) { payload[0]=i; assert(Store::append(payload,8,id) && id==i); }
    assert(!Store::append(payload,8,id) && id==0 && Store::lastResult()==R::FULL);
    assert(!Store::releaseThrough(5));
    assert(Store::releaseThrough(2));
    unsigned before=writes; assert(Store::checkpoint()); assert(writes>before);
    before=writes; assert(Store::checkpoint() && writes==before);
    assert(Store::append(payload,8,id) && id==5);
    assert(!Store::readById(1,output,sizeof(output)) && Store::lastResult()==R::NOT_FOUND);
    assert(Store::readNext(0,id,output,sizeof(output)) && id==3 && output[0]==3);
    assert(Store::begin(config)); assert(Store::releasedThrough()==2);
    uint8_t restored[16]; assert(Store::copyGeneration(restored) && !memcmp(restored,generation,16));
    auto wrong=config; wrong.payloadBytes=9;
    assert(!Store::begin(wrong) && Store::lastResult()==R::CONFIG_MISMATCH);
    assert(Store::begin(config));
    assert(Store::releaseThrough(3)); denyRename="/eb_records/meta";
    Store::prepareMaintenance(); assert(Store::isPaused() && Store::maintenanceResult()==R::IO_ERROR);
    assert(!Store::append(payload,8,id) && Store::lastResult()==R::PAUSED);
    assert(!Store::begin(config) && Store::lastResult()==R::PAUSED);
    Store::resumeAfterMaintenance(); denyRename.clear(); assert(Store::begin(config));
    assert(Store::releasedThrough()==2); // failed checkpoint must not release extra data
    assert(Store::releaseThrough(3)); Store::prepareMaintenance(); assert(Store::maintenanceResult()==R::OK);
    assert(!Store::readById(3,output,sizeof(output)) && Store::lastResult()==R::PAUSED);
    Store::resumeAfterMaintenance(); assert(Store::begin(config) && Store::releasedThrough()==3);
    assert(disk["/app/data"]==std::vector<uint8_t>({1,2,3}));

    // Every cut point in a single record: valid prefix survives, partial tail seals segment.
    reset(); assert(Store::append(payload,8,id) && id==1);
    const auto base=disk;
    for(int cut=0;cut<12;++cut) {
        disk=base; assert(Store::begin(config)); writeBudget=cut;
        assert(!Store::append(payload,8,id) && id==0 && !Store::isReady());
        writeBudget=-1; assert(Store::begin(config));
        assert(Store::readById(1,output,sizeof(output)));
        assert(Store::append(payload,8,id)); assert(id==uint64_t(cut ? 3 : 2));
    }
    // Any bit corruption of a complete record is fail-closed, including its CRC.
    for(size_t byte=48;byte<60;++byte) {
        disk=base; disk["/eb_records/s0"][byte]^=0x80;
        assert(!Store::begin(config) && Store::lastResult()==R::CORRUPT);
    }
    disk=base; disk["/eb_records/meta"][12]^=1;
    assert(!Store::begin(config) && Store::lastResult()==R::CORRUPT);
    disk=base; disk["/eb_records/s0"][24]^=1;
    assert(!Store::begin(config) && Store::lastResult()==R::CORRUPT);

    // Interrupted rotation before/after metadata commit keeps old unreclaimed facts.
    reset(); for(int i=0;i<4;++i) assert(Store::append(payload,8,id));
    assert(Store::releaseThrough(2)); const auto full=disk;
    for(int cut=0;cut<112;++cut) {
        disk=full; assert(Store::begin(config)); assert(Store::releaseThrough(2));
        writeBudget=cut; assert(!Store::append(payload,8,id));
        writeBudget=-1; assert(Store::begin(config));
        assert(Store::readById(3,output,sizeof(output)) && Store::readById(4,output,sizeof(output)));
        assert(Store::releaseThrough(2)); assert(Store::append(payload,8,id)); assert(id>=5);
    }
    // Repeated torn appends must not recycle the final complete fact: the upper layer
    // derives its next continuous sequence from that fact, not from physical ID holes.
    reset(); for(int i=0;i<4;++i) assert(Store::append(payload,8,id));
    assert(Store::releaseThrough(4)); assert(Store::checkpoint());
    for(int attempt=0;attempt<6;++attempt) {
        writeBudget=113; // meta64 + segment48 + one payload byte
        assert(!Store::append(payload,8,id)); writeBudget=-1;
        assert(Store::begin(config));
        assert(Store::readById(4,output,sizeof(output)));
    }
    assert(Store::append(payload,8,id) && id>4);
    assert(Store::readById(4,output,sizeof(output)));

    // Randomized-looking repeated rotation and reboot, no id reuse or stale-generation records.
    reset(); uint64_t previous=0;
    for(unsigned i=0;i<300;++i) {
        payload[0]=i; assert(Store::append(payload,8,id)); assert(id>previous); previous=id;
        assert(Store::readById(id,output,sizeof(output)) && !memcmp(payload,output,8));
        assert(Store::releaseThrough(id));
        if(i%3==0) { assert(Store::checkpoint()); assert(Store::begin(config)); }
    }
    availableBytes=8192; assert(!Store::append(payload,8,id) && Store::lastResult()==R::FS_RESERVE);
    availableBytes=1024*1024;
    assert(!Store::append(payload,7,id) && Store::lastResult()==R::INVALID_ARGUMENT);
    auto wide=config; wide.payloadBytes=256; generation[0]=2;
    assert(Store::rebuild(wide,generation)); memset(output,0xa5,sizeof(output));
    assert(Store::append(output,sizeof(output),id)); assert(Store::begin(wide));
    assert(Store::readById(id,output,sizeof(output)));
    for(uint8_t byte:output) assert(byte==0xa5);
    denyRemove="/eb_records/s0"; generation[0]=3;
    assert(!Store::rebuild(wide,generation)); assert(!Store::begin(wide) && Store::lastResult()==R::NOT_FOUND);
    puts("Record Store recovery, cut points, CRC, retention, maintenance, reserve and 300 append/release cycles passed");
    testImport();
}
