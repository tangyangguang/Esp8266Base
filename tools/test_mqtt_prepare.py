#!/usr/bin/env python3
"""Exercise production preparation + handle gate ordering with a fake transport boundary."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
source = (ROOT/'src/Esp8266BaseMQTT.cpp').read_text()
def method(signature):
    start=source.index(signature)
    opening=source.index('{',start)
    depth=1
    end=opening+1
    while depth:
        depth += (source[end]=='{')-(source[end]=='}')
        end+=1
    return source[start:end]
prelude=r'''
#include <cassert>
#define private public
#include "Esp8266BaseMQTT.h"
#undef private
#define ESP8266BASE_LOG_W(...)
#define ESP8266BASE_LOG_I(...)
using namespace Esp8266BaseMQTTInternal;
static uint32_t now=0;
uint32_t millis() { return now; }
static bool wifi=true, synced=true, transportOpen=false, transportSuccess=false;
static unsigned calls=0, connects=0, recoveryFailures=0;
static int lastTlsCode=0;
static bool permanentCertificateError(int) { return false; }
struct Esp8266BaseWiFi { static bool isConnected() { return wifi; } };
struct Esp8266BaseNTP { static bool isSynced() { return synced; } };
Esp8266BaseMQTTPrepareCallback Esp8266BaseMQTT::_prepareCallback=nullptr;
bool Esp8266BaseMQTT::_configured=true, Esp8266BaseMQTT::_begun=false;
bool Esp8266BaseMQTT::_shutdownActive=false, Esp8266BaseMQTT::_reconnectRequested=false;
bool Esp8266BaseMQTT::_connectAttemptsEnabled=true;
Esp8266BaseMQTTState Esp8266BaseMQTT::_state=Esp8266BaseMQTTState::BACKOFF;
Esp8266BaseMQTTDisconnectReason Esp8266BaseMQTT::_lastReason=Esp8266BaseMQTTDisconnectReason::NONE;
uint32_t Esp8266BaseMQTT::_attemptCount=0, Esp8266BaseMQTT::_retryAt=0, Esp8266BaseMQTT::_retryDelay=2000;
char Esp8266BaseMQTT::_willTopic[129]="topic";
const uint8_t* Esp8266BaseMQTT::_willPayload=nullptr;
size_t Esp8266BaseMQTT::_willLength=0;
uint8_t Esp8266BaseMQTT::_willQos=1;
bool Esp8266BaseMQTT::_willRetain=true;
void Esp8266BaseMQTT::_handleShutdown() {}
void Esp8266BaseMQTT::_handleEscalatedRecovery(uint32_t) {}
void Esp8266BaseMQTT::_disconnectForGate(Esp8266BaseMQTTState state) { _state=state; }
void Esp8266BaseMQTT::_closeTransport(Esp8266BaseMQTTDisconnectReason,bool,bool) { transportOpen=false; }
bool Esp8266BaseMQTT::_pumpTransport() { return true; }
bool Esp8266BaseMQTT::_connectTransport() { ++connects; transportOpen=transportSuccess; return transportSuccess; }
void Esp8266BaseMQTT::_noteRecoveryFailure(Esp8266BaseMQTTDisconnectReason) { ++recoveryFailures; }
static unsigned mode=0;
static uint8_t bytes[32];
static char oversized[130];
bool prepare(Esp8266BaseMQTTWill& will) {
    ++calls;
    if(mode==1) return false;
    if(mode==2) { will.length=ESP8266BASE_MQTT_MAX_PAYLOAD_BYTES+1; return true; }
    if(mode==3) { will.topic=oversized; return true; }
    bytes[0]=calls; will.payload=bytes; will.length=calls; will.qos=1; return true;
}
'''
main=r'''
int main() {
    using M=Esp8266BaseMQTT;
    memset(oversized,'x',129); oversized[129]=0;
    assert(M::setPrepareCallback(prepare)); M::_begun=true;
    assert(!M::setPrepareCallback(nullptr));
    wifi=false; M::handle(); assert(!calls && !connects);
    wifi=true; synced=false; M::handle(); assert(!calls && !connects);
    synced=true; M::_connectAttemptsEnabled=false; M::handle(); assert(!calls && !connects);
    M::_connectAttemptsEnabled=true; M::handle(); assert(calls==1 && connects==1 && recoveryFailures==1);
    assert(M::_willPayload==bytes && M::_willLength==1 && bytes[0]==1 && !strcmp(M::_willTopic,"topic"));
    M::handle(); assert(calls==1); // retry not due
    now=M::_retryAt; M::handle(); assert(calls==2 && connects==2 && M::_willLength==2);
    for(mode=1;mode<=3;++mode) {
        now=M::_retryAt; M::handle();
        assert(connects==2 && recoveryFailures==2 && M::_willLength==2);
        assert(M::_lastReason==Esp8266BaseMQTTDisconnectReason::PREPARE_REJECTED);
        assert(!strcmp(M::lastDisconnectReasonName(),"prepare_rejected"));
    }
    mode=0; now=M::_retryAt; transportSuccess=true; M::handle();
    assert(connects==3 && calls==6 && M::_willLength==6);
    M::handle(); assert(calls==6); // established session does not re-prepare
    transportOpen=false; M::_prepareCallback=nullptr; M::_retryAt=now;
    M::handle(); assert(connects==4 && calls==6 && M::_willLength==6);
}
'''
with tempfile.TemporaryDirectory(prefix='esp8266-mqtt-prepare-') as temp:
    directory=Path(temp)
    (directory/'test.cpp').write_text(prelude+'\n'.join(method(s) for s in [
        'bool Esp8266BaseMQTT::setPrepareCallback(', 'bool Esp8266BaseMQTT::_prepareWill()',
        'void Esp8266BaseMQTT::handle()', 'void Esp8266BaseMQTT::_scheduleRetry()',
        'bool Esp8266BaseMQTT::_isDue(', 'const char* Esp8266BaseMQTT::lastDisconnectReasonName()'])+main)
    subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-I',str(ROOT/'src'),
        '-I',str(ROOT/'tools/native_record_store'),'-DESP8266BASE_USE_MQTT=1',
        '-DESP8266BASE_USE_NTP=1','-DESP8266BASE_USE_WEB=0','-DESP8266BASE_USE_JOURNAL=0',
        str(directory/'test.cpp'),'-o',str(directory/'test')],check=True)
    subprocess.run([str(directory/'test')],check=True)
print('Production MQTT prepare and handle gates: retries, rejection, bounds and borrowing passed')
