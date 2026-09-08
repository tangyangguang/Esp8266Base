#pragma once
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <stdint.h>
extern std::map<std::string,std::vector<uint8_t>> disk;
extern std::string denyRename, denyRemove;
extern int writeBudget;
extern unsigned writes;
extern size_t availableBytes;
enum SeekMode { SeekSet };
struct FSInfo { size_t totalBytes, usedBytes; };
struct File {
    std::string path; size_t position=0; bool valid=false;
    explicit operator bool() const { return valid; }
    size_t size() const { return valid ? disk.at(path).size() : 0; }
    size_t write(const uint8_t* data,size_t count) {
        if (!valid) return 0;
        ++writes;
        if (writeBudget>=0) { count=std::min(count,size_t(writeBudget)); writeBudget-=count; }
        auto& bytes=disk[path]; if(position+count>bytes.size()) bytes.resize(position+count);
        memcpy(bytes.data()+position,data,count); position+=count; return count;
    }
    int read(uint8_t* data,size_t count) {
        if (!valid) return -1;
        auto& bytes=disk[path]; count=std::min(count,bytes.size()-position);
        memcpy(data,bytes.data()+position,count); position+=count; return int(count);
    }
    bool seek(size_t offset,SeekMode) { if(!valid || offset>size()) return false; position=offset; return true; }
    void flush() {} void close() { valid=false; }
};
struct FakeLittleFS {
    bool info(FSInfo& info) { info={1024*1024,1024*1024-availableBytes}; return true; }
    bool exists(const char* p) { return std::string(p)=="/eb_records" || disk.count(p); }
    bool mkdir(const char*) { return true; }
    bool remove(const char* p) { return denyRemove==p ? false : disk.erase(p)>0; }
    bool rename(const char* a,const char* b) {
        if(denyRename==b || !disk.count(a)) return false;
        disk[b]=disk[a]; disk.erase(a); return true;
    }
    File open(const char* path,const char* mode) {
        File f; f.path=path;
        if(mode[0]=='w') disk[path].clear();
        if(mode[0]=='a' && !disk.count(path)) disk[path]={};
        f.valid=disk.count(path); if(mode[0]=='a' && f.valid) f.position=f.size(); return f;
    }
};
extern FakeLittleFS LittleFS;
