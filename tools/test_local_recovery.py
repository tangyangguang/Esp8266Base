#!/usr/bin/env python3
"""Compile production Config/mDNS with bounded host filesystem and SDK fakes."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HEADERS = {
    'Arduino.h': '''#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
using PGM_P = const char*;
#define PSTR(x) (x)
uint32_t millis();
void yield();
''',
    'LittleFS.h': '''#pragma once
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
extern std::map<std::string,std::string> disk;
extern std::string failedRemove;
struct File {
    std::string path; bool valid=false;
    explicit operator bool() const { return valid; }
    size_t print(const char* s) { disk[path]=s; return strlen(s); }
    size_t readBytes(char* p,size_t n) { n=std::min(n,disk[path].size()); memcpy(p,disk[path].data(),n); return n; }
    void flush() {} void close() {}
};
struct Dir {
    std::vector<std::string> names; size_t index=0;
    bool next() { if(index==names.size()) return false; ++index; return true; }
    std::string fileName() { return names[index-1]; }
};
struct FakeFS {
    bool exists(const char* p) { return disk.count(p); }
    bool remove(const char* p) { return failedRemove==p ? false : disk.erase(p)>0; }
    bool rename(const char* a,const char* b) { if(!disk.count(a)) return false; disk[b]=disk[a]; disk.erase(a); return true; }
    File open(const char* p,const char* mode) { File f; f.path=p; if(mode[0]=='w') disk[p]=""; f.valid=disk.count(p); return f; }
    Dir openDir(const char*) { Dir d; for(const auto& x:disk) d.names.push_back(x.first); return d; }
};
extern FakeFS LittleFS;
''',
    'ESP8266mDNS.h': '''#pragma once
struct FakeMDNS {
    bool allowed=false; unsigned calls=0,services=0,updates=0;
    bool begin(const char*) { ++calls; return allowed; }
    void addService(const char*,const char*,int) { ++services; }
    void update() { ++updates; }
};
extern FakeMDNS MDNS;
''',
}
MAIN = r'''
#include <cassert>
#include "src/Esp8266BaseConfig.cpp"
#include "src/Esp8266BaseMDNS.cpp"
uint32_t nowMs=0;
uint32_t millis() { return nowMs; }
void yield() {}
std::map<std::string,std::string> disk;
std::string failedRemove;
FakeFS LittleFS;
FakeMDNS MDNS;
bool Esp8266BaseFilesystem::isReady() { return true; }
void Esp8266BaseLog::log(uint8_t,const char*,const char*,...) {}
void Esp8266BaseLog::log_P(uint8_t,const char*,PGM_P,...) {}
int main() {
    assert(Esp8266BaseConfig::begin());
    disk["/cfg_a"]="1"; disk["/cfg_z"]="2"; disk["/app/data"]="keep";
    assert(Esp8266BaseConfig::setIntDeferred("a",3));
    assert(Esp8266BaseConfig::setIntDeferred("z",4));
    failedRemove="/cfg_z";
    assert(!Esp8266BaseConfig::clearAll());
    assert(!disk.count("/cfg_a") && disk.count("/cfg_z"));
    assert(Esp8266BaseConfig::pendingCount()==2);
    assert(Esp8266BaseConfig::flush());
    assert(disk["/cfg_a"]=="3" && disk["/cfg_z"]=="4");
    assert(Esp8266BaseConfig::setIntDeferred("a",5));
    disk["/cfg_"+std::string(24,'k')+".tmp"]="partial";
    disk["/cfg_"+std::string(24,'k')+".bak"]="backup";
    failedRemove.clear(); assert(Esp8266BaseConfig::clearAll());
    assert(Esp8266BaseConfig::pendingCount()==0);
    assert(disk.size()==1 && disk["/app/data"]=="keep");
    assert(Esp8266BaseConfig::clearAll());

    assert(!Esp8266BaseMDNS::begin(nullptr)); assert(MDNS.calls==0);
    assert(!Esp8266BaseMDNS::begin("device"));
    for(nowMs=1;nowMs<5000;++nowMs) assert(!Esp8266BaseMDNS::begin("device"));
    assert(MDNS.calls==1 && MDNS.services==0 && !Esp8266BaseMDNS::isRunning());
    MDNS.allowed=true; assert(Esp8266BaseMDNS::begin("device"));
    assert(MDNS.calls==2 && MDNS.services==1 && Esp8266BaseMDNS::isRunning());
    Esp8266BaseMDNS::handle(); assert(MDNS.updates==1);
    MDNS.allowed=false; nowMs=UINT32_MAX-2499;
    assert(!Esp8266BaseMDNS::begin("device")); assert(!Esp8266BaseMDNS::isRunning());
    Esp8266BaseMDNS::handle(); assert(MDNS.updates==1);
    nowMs=2499; assert(!Esp8266BaseMDNS::begin("device")); assert(MDNS.calls==3);
    nowMs=2500; MDNS.allowed=true; assert(Esp8266BaseMDNS::begin("device")); assert(MDNS.calls==4);
}
'''
with tempfile.TemporaryDirectory(prefix='esp8266-local-recovery-') as temp:
    root = Path(temp)
    for name, content in HEADERS.items():
        (root/name).write_text(content)
    (root/'test.cpp').write_text(MAIN)
    subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-I',str(root),'-I',str(ROOT),
                    '-DESP8266BASE_LOG_LEVEL=0',str(root/'test.cpp'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True)
# The facade must propagate begin failure to allow the tested retry path.
assert '_mdnsWasStarted = Esp8266BaseMDNS::begin(_hostname);' in (ROOT/'src/Esp8266Base.cpp').read_text()
print('Production Config partial-clear recovery and mDNS failure/retry/reconnect/wraparound passed')
