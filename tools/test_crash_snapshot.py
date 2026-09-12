#!/usr/bin/env python3
"""Bounded host check of the real crash hook, RTC layout and integrity; no hardware."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="base-crash-") as raw:
    temp = Path(raw)
    (temp / "user_interface.h").write_text('''#pragma once
#include <stdint.h>
struct rst_info { uint32_t reason, exccause, epc1, epc2, epc3, excvaddr, depc; };
bool system_rtc_mem_read(uint32_t, void*, uint32_t);
bool system_rtc_mem_write(uint32_t, const void*, uint32_t);
''')
    (temp / "test.cpp").write_text('''#include "Esp8266BaseCrash.h"
extern "C" {
#include "user_interface.h"
void custom_crash_callback(rst_info*, uint32_t, uint32_t);
}
#include <cassert>
#include <cstring>
uint32_t rtc[192];
bool system_rtc_mem_read(uint32_t word, void* p, uint32_t size) {
 assert(word==72 && size==112); memcpy(p, rtc+word, size); return true;
}
bool system_rtc_mem_write(uint32_t word, const void* p, uint32_t size) {
 assert(word==72 && size==112); memcpy(rtc+word, p, size); return true;
}
int main() {
 memset(rtc, 0x5a, sizeof(rtc));
 Esp8266BaseCrash::setBuildId(0x12345678, 0xabcdef12);
 Esp8266BaseCrash::begin(41); assert(!Esp8266BaseCrash::last());
 Esp8266BaseCrash::setPhase(35);
 { Esp8266BaseCrashPhase scope(36); assert(Esp8266BaseCrash::currentPhase()==36); }
 assert(Esp8266BaseCrash::currentPhase()==35);
 rst_info info{2,9,0x40201010,0,0,0x100,0};
 custom_crash_callback(&info, 0, 0); // Invalid stack range must never be dereferenced.
 Esp8266BaseCrash::begin(42);
 const auto* s=Esp8266BaseCrash::last();
 assert(s && Esp8266BaseCrash::pending() && s->bootCount==41 && s->phase==35);
 assert(s->buildId[0]==0x12345678 && s->buildId[1]==0xabcdef12);
 assert(s->exceptionCause==9 && s->epc1==0x40201010 && !s->frameCount);
 assert(Esp8266BaseCrash::markArchived() && !Esp8266BaseCrash::pending());
 for(unsigned n=0;n<192;++n) if(n<72 || n>=100) assert(rtc[n]==0x5a5a5a5a);
 assert(esp8266CrashCodeAddress(0x40201010));
 assert(!esp8266CrashCodeAddress(0x3ffff000)); // Never persist arbitrary data words.
 assert(!esp8266CrashCodeAddress(0x40400000));
 rtc[79]^=1; Esp8266BaseCrash::begin(43); assert(!Esp8266BaseCrash::last());
 custom_crash_callback(&info, 0x3ffe8001, 0x3ffe8100); // Unaligned range.
 Esp8266BaseCrash::begin(44); assert(Esp8266BaseCrash::last()->frameCount==0);
}
''')
    binary = temp / "test"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-DESP8266BASE_USE_CRASH=1",
                    "-I" + str(temp), "-I" + str(ROOT / "tools/native_record_store"),
                    "-I" + str(ROOT / "src"), str(temp / "test.cpp"),
                    str(ROOT / "src/Esp8266BaseCrash.cpp"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Crash hook: RTC isolation, exact identity, registers, phase, CRC and invalid stack bounds passed")
