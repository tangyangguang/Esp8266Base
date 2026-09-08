#include <cassert>
#include <cstring>
#include "../src/Esp8266BaseNTPPacket.h"
#include "../src/Esp8266BaseStreamWrite.h"

static uint32_t nowMs;
struct FakeClient {
    bool online = true;
    size_t limit = 2;
    uint32_t cost = 0;
    char received[32] = {};
    size_t used = 0;
    bool connected() { return online; }
    size_t write(const uint8_t* data, size_t length) {
        nowMs += cost;
        const size_t count = length < limit ? length : limit;
        memcpy(received + used, data, count);
        used += count;
        return count;
    }
};

static void putTimestamp(uint8_t* target, uint32_t epoch, uint32_t fraction=0) {
    const uint32_t seconds = epoch + 2208988800UL;
    for (int i=0; i<4; ++i) {
        target[i] = static_cast<uint8_t>(seconds >> ((3-i)*8));
        target[i+4] = static_cast<uint8_t>(fraction >> ((3-i)*8));
    }
}

int main() {
    uint8_t packet[48] = {};
    uint8_t nonce[8] = {1,2,3,4,5,6,7,8};
    memcpy(packet+24, nonce, sizeof(nonce));
    assert(Esp8266BaseNTPInternal::matchesRequest(packet,nonce));
    ++packet[24];
    assert(!Esp8266BaseNTPInternal::matchesRequest(packet,nonce));
    assert(Esp8266BaseNTPInternal::timestampMicros(packet+40)==0);
    putTimestamp(packet+40, 1800000000UL, 0x80000000UL);
    assert(Esp8266BaseNTPInternal::timestampMicros(packet+40)==1800000000500000ULL);
    putTimestamp(packet+40, 2200000000UL); // NTP seconds have wrapped after 2036.
    assert(Esp8266BaseNTPInternal::timestampMicros(packet+40)==2200000000000000ULL);
    putTimestamp(packet+40, 2085978495UL, 0x80000000UL);
    assert(Esp8266BaseNTPInternal::timestampMicros(packet+40)==2085978495500000ULL);

    const uint8_t data[] = "abcdefgh";
    auto clock=[]() { return nowMs; };
    auto cooperate=[]() { ++nowMs; };
    FakeClient client;
    nowMs=0;
    assert(Esp8266BaseInternal::writeAll(client,data,8,0,30,clock,cooperate));
    assert(client.used==8 && memcmp(client.received,data,8)==0);
    client={}; client.cost=10; nowMs=0;
    assert(!Esp8266BaseInternal::writeAll(client,data,8,0,30,clock,cooperate));
    assert(client.used<8);
    client={}; client.limit=0; nowMs=0;
    assert(!Esp8266BaseInternal::writeAll(client,data,8,0,30,clock,cooperate));
    assert(nowMs==30);
    client={}; client.online=false; nowMs=0;
    assert(!Esp8266BaseInternal::writeAll(client,data,8,0,30,clock,cooperate));
    assert(client.used==0);
    client={}; client.cost=4; nowMs=0xfffffffaUL;
    assert(Esp8266BaseInternal::writeAll(client,data,8,0xfffffffaUL,30,clock,cooperate));
    client={}; client.cost=10; nowMs=0;
    assert(Esp8266BaseInternal::writeAll(client,data,2,0,30,clock,cooperate));
    assert(Esp8266BaseInternal::writeAll(client,data,2,0,30,clock,cooperate));
    assert(!Esp8266BaseInternal::writeAll(client,data,2,0,30,clock,cooperate));
    return 0;
}
