#include "Esp8266BaseOptions.h"
#if ESP8266BASE_USE_NTP
#include "Esp8266BaseNTP.h"
#include "Esp8266BaseNTPPacket.h"
#include "Esp8266BaseNtpCore.h"
extern "C" {
#include <osapi.h>
}
#include "Esp8266BaseLog.h"
#if ESP8266BASE_USE_WATCHDOG
#include "Esp8266BaseWatchdog.h"
#endif
#if ESP8266BASE_USE_JOURNAL
#include "Esp8266BaseJournal.h"
#endif
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <sntp.h>
#include <sys/time.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

namespace core = Esp8266BaseNtpCore;

// ----------------------------------------------------------------------------
// 静态成员定义
// ----------------------------------------------------------------------------
bool     Esp8266BaseNTP::_clockTrusted = false;
bool     Esp8266BaseNTP::_timeSuspect = false;
bool     Esp8266BaseNTP::_logSwitched = false;
bool     Esp8266BaseNTP::_uncertaintyMeasured = false;
bool     Esp8266BaseNTP::_waiting = false;
bool     Esp8266BaseNTP::_activityActive = false;
bool     Esp8266BaseNTP::_refreshResolverAfterReconnect = false;
uint8_t  Esp8266BaseNTP::_mode = static_cast<uint8_t>(Mode::ACQUIRE);
uint8_t  Esp8266BaseNTP::_serverIndex = 0;
uint8_t  Esp8266BaseNTP::_attempts = 0;
uint16_t Esp8266BaseNTP::_estimatedUncertaintyMs = 0;
uint32_t Esp8266BaseNTP::_startedMs = 0;
uint32_t Esp8266BaseNTP::_lastCheckMs = 0;
uint32_t Esp8266BaseNTP::_lastPendingLogMs = 0;
uint32_t Esp8266BaseNTP::_nextActionMs = 0;
uint32_t Esp8266BaseNTP::_sentMs = 0;
uint32_t Esp8266BaseNTP::_lastSyncRttMs = 0;

static WiFiUDP _ntpUdp;
static IPAddress _serverIp[2];          // 启动期解析并缓存，运行期校验零 DNS
static uint8_t _nonce[8];
static core::Sample _sample[2];
static const uint16_t NTP_PORT = 123;
static const uint16_t NTP_LOCAL_PORT = 2390;
static const uint32_t NTP_RESPONSE_TIMEOUT_MS = 3000;

// NTP 服务器名（全部在 Flash）
static const char NTP_S1[] PROGMEM = ESP8266BASE_NTP_SERVER_1;
static const char NTP_S2[] PROGMEM = ESP8266BASE_NTP_SERVER_2;
static const char NTP_S3[] PROGMEM = ESP8266BASE_NTP_SERVER_3;
static const char* const NTP_SERVERS[] PROGMEM = { NTP_S1, NTP_S2, NTP_S3 };

// ----------------------------------------------------------------------------
// begin — WiFi（重）连接后触发；设时区、拒绝 DHCP 时间源，幂等
// ----------------------------------------------------------------------------
bool Esp8266BaseNTP::begin() {
    char tz[12];
    core::formatPosixTz(tz, sizeof(tz), ESP8266BASE_NTP_TIMEZONE);
    setenv("TZ", tz, 1);  // POSIX 与习惯相反：UTC+8 -> UTC-8
    tzset();
    // 纵深防御：本固件从不 sntp_init/configTime，lwIP SNTP 根本不运行；
    // 这里再显式关闭“从 DHCP 拿 NTP”，防止任何后续路径误启用。
    sntp_servermode_dhcp(0);

    _startedMs = millis();
    _lastPendingLogMs = 0;
    _lastCheckMs = 0;
    _waiting = false;
    _serverIndex = 0;
    if (!_clockTrusted) {
        _attempts = 0;  // 重连后给未获信阶段一个新鲜退避
        // 未获信：每次重连都重新解析，供双源采集。
        _serverIp[0] = IPAddress();
        _serverIp[1] = IPAddress();
    } else if (_refreshResolverAfterReconnect) {
        // 上一轮校验因网络/DNS/超时失败：重连后刷新一次解析（应对服务器 IP
        // 变更或漫游到异构网络），然后清除标记。
        _serverIp[0] = IPAddress();
        _serverIp[1] = IPAddress();
        _refreshResolverAfterReconnect = false;
    }
    // 已获信且上轮校验成功：复用缓存 IP，稳态重连零同步 DNS，避免弱网下
    // hostByName 秒级阻塞卡住同循环其后的 Web 服务（对齐旧后台 SNTP）。
    // 已获可信时间后重连：不重新设钟，安排一次运行期校验即可。
    _nextActionMs = _startedMs + 1000UL;

    ESP8266BASE_LOG_I_P("NTP ", "ntp_client_started timezone=%s servers=%s,%s set_clock=dual_source_consensus dhcp_ntp=off trusted=%s",
                      tz, ESP8266BASE_NTP_SERVER_1, ESP8266BASE_NTP_SERVER_2,
                      _clockTrusted ? "yes" : "no");
    return true;
}

// ----------------------------------------------------------------------------
// 活动输出闸门
// ----------------------------------------------------------------------------
void Esp8266BaseNTP::setActivityActive(bool active) {
    // 仅在状态边沿动作；连续多圈 true 不重复 stop UDP、不推进退避。
    if (_activityActive == active) return;
    _activityActive = active;
    if (active) {
        // 安静挂起：只释放可能在途的 UDP，绝不计一次失败或改变退避/可信闘锁。
        if (_waiting) {
            _ntpUdp.stop();
            _waiting = false;
        }
    } else {
        // 活动结束后尽快重新采集/校验一次（保留的 _nextActionMs 可能远在将来）。
        _lastCheckMs = 0;
        _nextActionMs = millis() + 200UL;
    }
}

bool Esp8266BaseNTP::activityActive() { return _activityActive; }

// ----------------------------------------------------------------------------
// handle
// ----------------------------------------------------------------------------
void Esp8266BaseNTP::handle() {
    const uint32_t now = millis();

    // 安全关键活动运行中：不发起 DNS/UDP，避免弱网同步解析阻塞主循环。
    // 在途请求已在 true 边沿释放；这里直接返回，不采样 _lastCheckMs，
    // 使 false 边沿安排的 200ms 采集不被 1s 节流推迟。
    if (_activityActive) return;

    // 在途请求：每次 loop 都 poll/判超时，RTT 需要毫秒精度，绝不能被下面的
    // 1 秒空闲节流量化（否则 RTT 会被高估到约 1s 而撞上 500ms 信任门）。
    if (_waiting) {
        if (_pollResponse()) {
            _advanceOrFinish(now);
            return;
        }
        if (now - _sentMs >= NTP_RESPONSE_TIMEOUT_MS)
            _abortCycle(now, "response_timeout");
        return;
    }

    if (now - _lastCheckMs < 1000UL) return;  // 空闲调度 1s 节流
    _lastCheckMs = now;

    if (_isDue(now, _nextActionMs)) {
        _startCycle(now, _clockTrusted ? Mode::VERIFY : Mode::ACQUIRE);
    } else if (!_clockTrusted &&
               (now - _lastPendingLogMs >= 30000UL || _lastPendingLogMs == 0)) {
        _lastPendingLogMs = now;
        ESP8266BASE_LOG_W_P("NTP ", "ntp_trust_pending elapsed=%lus attempts=%u",
                          (unsigned long)((now - _startedMs) / 1000UL),
                          static_cast<unsigned>(_attempts));
    }
}

void Esp8266BaseNTP::reset() {
    _ntpUdp.stop();
    _waiting = false;
    // 不触碰 _activityActive：它表达业务输出状态，与 WiFi 连接状态正交。
    // 关键：不清除 _clockTrusted/_timeSuspect。墙钟继续走，重连不重新设钟。
    ESP8266BASE_LOG_I_P("NTP ", "ntp_client_reset reason=wifi_disconnected trusted=%s",
                      _clockTrusted ? "kept" : "not_yet");
}

// ----------------------------------------------------------------------------
// 双源采集状态机
// ----------------------------------------------------------------------------
void Esp8266BaseNTP::_startCycle(uint32_t now, Mode mode) {
    _mode = static_cast<uint8_t>(mode);
    _serverIndex = 0;
    _sample[0] = core::Sample{};
    _sample[1] = core::Sample{};
    if (!_ntpUdp.begin(NTP_LOCAL_PORT)) {
        _abortCycle(now, "udp_bind_failed");
        return;
    }
    _requestServer(0, now);
}

const char* Esp8266BaseNTP::_serverName(uint8_t index) {
    static char name[32];
    strncpy_P(name, reinterpret_cast<const char*>(pgm_read_ptr(&NTP_SERVERS[index])),
              sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    return name;
}

bool Esp8266BaseNTP::_requestServer(uint8_t index, uint32_t now) {
    if (index >= 2) return false;
    const char* server = _serverName(index);

#if ESP8266BASE_USE_WATCHDOG
    Esp8266BaseWatchdog::feed();
#endif
    bool resolved;
    if (static_cast<Mode>(_mode) == Mode::VERIFY && _serverIp[index].isSet()) {
        resolved = true;  // 运行期校验复用启动期解析的 IP，避免 TLS 长连接期间 DNS 阻塞
    } else {
        resolved = WiFi.hostByName(server, _serverIp[index]) == 1 && _serverIp[index].isSet();
    }
#if ESP8266BASE_USE_WATCHDOG
    Esp8266BaseWatchdog::feed();
#endif
    if (!resolved) {
        ESP8266BASE_LOG_W_P("NTP ", "ntp_dns_failed server=%s index=%u", server,
                          static_cast<unsigned>(index));
        _abortCycle(now, "dns_failed");
        return false;
    }

    uint8_t pkt[48];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x1B;  // LI=0, VN=3, Mode=3(client)
    const uint32_t nonceHigh = os_random();
    const uint32_t nonceLow = os_random();
    memcpy(_nonce, &nonceHigh, sizeof(nonceHigh));
    memcpy(_nonce + 4, &nonceLow, sizeof(nonceLow));
    _nonce[0] |= 1U;
    memcpy(pkt + 40, _nonce, sizeof(_nonce));  // 服务器回显到 Originate(偏移24)做关联

    const bool packetReady =
        _ntpUdp.beginPacket(_serverIp[index], NTP_PORT) &&
        _ntpUdp.write(pkt, sizeof(pkt)) == sizeof(pkt);
    if (packetReady && _ntpUdp.endPacket()) {
        _sentMs = millis();  // RTT 从实际发出起算，排除 DNS 与组包耗时
        _waiting = true;
        _serverIndex = index;
        ESP8266BASE_LOG_I_P("NTP ", "ntp_request server=%s ip=%s index=%u mode=%s",
                          server, _serverIp[index].toString().c_str(),
                          static_cast<unsigned>(index),
                          static_cast<Mode>(_mode) == Mode::VERIFY ? "verify" : "acquire");
        return true;
    }
    ESP8266BASE_LOG_W_P("NTP ", "ntp_send_failed server=%s", server);
    _abortCycle(now, "send_failed");
    return false;
}

bool Esp8266BaseNTP::_pollResponse() {
    const uint32_t now = millis();  // 实时采样，避免沿用 handle 入口时刻而高估 RTT
    // 超窗的迟到包不采纳（超时判定先于任何收包）。
    if (now - _sentMs >= NTP_RESPONSE_TIMEOUT_MS) return false;
    const int packetSize = _ntpUdp.parsePacket();
    if (packetSize < 48) return false;
    const IPAddress remoteIp = _ntpUdp.remoteIP();
    const uint16_t remotePort = _ntpUdp.remotePort();
    uint8_t pkt[48];
    if (_ntpUdp.read(pkt, sizeof(pkt)) != sizeof(pkt)) return false;

    const uint8_t mode = pkt[0] & 0x07;
    const uint8_t leap = (pkt[0] >> 6) & 0x03;
    const uint8_t stratum = pkt[1];
    const uint8_t index = _serverIndex;
    if (remoteIp != _serverIp[index] || remotePort != NTP_PORT || mode != 4 ||
        leap == 3 || stratum == 0 || stratum > 15 || ((pkt[0] >> 3) & 7) != 3 ||
        !Esp8266BaseNTPInternal::matchesRequest(pkt, _nonce)) {
        ESP8266BASE_LOG_W_P("NTP ", "ntp_packet_rejected remote=%s port=%u expected=%s mode=%u leap=%u stratum=%u",
                          remoteIp.toString().c_str(), static_cast<unsigned>(remotePort),
                          _serverIp[index].toString().c_str(), static_cast<unsigned>(mode),
                          static_cast<unsigned>(leap), static_cast<unsigned>(stratum));
        return false;  // 非当前等待源的合法响应：丢弃，继续等超时
    }

    const uint64_t receivedUs = Esp8266BaseNTPInternal::timestampMicros(pkt + 32);
    const uint64_t transmittedUs = Esp8266BaseNTPInternal::timestampMicros(pkt + 40);
    if (!(transmittedUs >= 1000000000ULL * 1000000ULL &&
          (receivedUs == 0 || transmittedUs >= receivedUs)))
        return false;

    const uint32_t rttMs = now - _sentMs;
    const uint64_t rttUs = uint64_t(rttMs) * 1000ULL;
    const uint64_t serverProcUs = receivedUs == 0 ? 0 : transmittedUs - receivedUs;
    const uint64_t networkUs = rttUs > serverProcUs ? rttUs - serverProcUs : 0;
    const uint64_t estimateUs = transmittedUs + networkUs / 2ULL;

    _sample[index] = core::Sample{
        static_cast<uint32_t>(_serverIp[index]), now,
        static_cast<uint32_t>(estimateUs / 1000000ULL),
        static_cast<uint16_t>((estimateUs % 1000000ULL) / 1000ULL),
        static_cast<uint16_t>(rttMs)};
    _waiting = false;
    return true;
}

void Esp8266BaseNTP::_advanceOrFinish(uint32_t now) {
    if (_serverIndex == 0) {
        _requestServer(1, now);
        return;
    }
    if (static_cast<Mode>(_mode) == Mode::ACQUIRE)
        _finishAcquire(now);
    else
        _finishVerify(now);
}

void Esp8266BaseNTP::_finishAcquire(uint32_t now) {
    uint64_t trustedAtB = 0;
    const core::Verdict verdict =
        core::evaluateAcquisition(_sample[0], _sample[1], trustedAtB);
    if (verdict != core::Verdict::TRUSTED) {
        _abortCycle(now, "sources_untrusted");
        return;
    }

    // 把可信时间从样本 b 接收时刻推进到设置瞬间再 settimeofday。
    const uint64_t setUs = trustedAtB + uint64_t(now - _sample[1].atMs) * 1000ULL;
    timeval tv;
    tv.tv_sec = static_cast<time_t>(setUs / 1000000ULL);
    tv.tv_usec = static_cast<suseconds_t>(setUs % 1000000ULL);
    if (settimeofday(&tv, nullptr) != 0) {
        _abortCycle(now, "settime_failed");
        return;
    }

    _clockTrusted = true;
    _timeSuspect = false;
    _uncertaintyMeasured = true;
    _lastSyncRttMs = (uint32_t(_sample[0].rttMs) + _sample[1].rttMs) / 2U;
    _estimatedUncertaintyMs =
        core::agreeWindowMs(_sample[0].rttMs, _sample[1].rttMs);
    _attempts = 0;
    _nextActionMs = now + uint32_t(ESP8266BASE_NTP_SYNC_INTERVAL) * 1000UL;
    _finishSync(time(nullptr));
    ESP8266BASE_LOG_I_P("NTP ", "ntp_trusted rtt0=%lums rtt1=%lums agree=%ums",
                      static_cast<unsigned long>(_sample[0].rttMs),
                      static_cast<unsigned long>(_sample[1].rttMs),
                      static_cast<unsigned>(_estimatedUncertaintyMs));
}

void Esp8266BaseNTP::_finishVerify(uint32_t now) {
    timeval tv;
    gettimeofday(&tv, nullptr);
    const uint64_t localUs =
        uint64_t(tv.tv_sec) * 1000000ULL + static_cast<uint64_t>(tv.tv_usec);
    uint64_t trustedNow = 0;
    const core::Verdict verdict =
        core::evaluateVerification(_sample[0], _sample[1], localUs, now, trustedNow);

    _attempts = 0;
    _nextActionMs = now + uint32_t(ESP8266BASE_NTP_SYNC_INTERVAL) * 1000UL;
    const long deltaMs = static_cast<long>((trustedNow > localUs ? trustedNow - localUs
                                                                : localUs - trustedNow) /
                                           1000ULL);
    if (verdict == core::Verdict::SUSPECT) {
        _timeSuspect = true;
        // 只标记/留证，绝不回拨或前拨墙钟（回拨会永久冻结命令账本）。
        ESP8266BASE_LOG_W_P("NTP ", "ntp_time_suspect clock_delta_ms=%ld (clock left unchanged)",
                          deltaMs);
    } else if (verdict == core::Verdict::TRUSTED) {
        if (_timeSuspect) _timeSuspect = false;
        ESP8266BASE_LOG_I_P("NTP ", "ntp_time_verified clock_delta_ms=%ld", deltaMs);
    } else {
        ESP8266BASE_LOG_W_P("NTP ", "ntp_verify_inconclusive (clock left unchanged)");
    }
    _ntpUdp.stop();  // 周期校验只在采集的几秒内占用 UDP:2390，结束立即释放
}

void Esp8266BaseNTP::_abortCycle(uint32_t now, const char* reason) {
    _waiting = false;
    _ntpUdp.stop();  // 退避/失败期间不常驻监听，下轮 _startCycle 重新 begin
    _sample[0] = core::Sample{};
    _sample[1] = core::Sample{};
    if (static_cast<Mode>(_mode) == Mode::VERIFY) {
        // 运行期校验遇网络/DNS 问题：不是不信任，下个正常周期再校验，不快速重试。
        // 网络类失败标记下一次 WiFi 重连刷新解析；时间不一致（sources_untrusted）
        // 与服务器 IP 无关，不刷新。
        if (reason && (strcmp(reason, "dns_failed") == 0 ||
                       strcmp(reason, "send_failed") == 0 ||
                       strcmp(reason, "response_timeout") == 0)) {
            _refreshResolverAfterReconnect = true;
        }
        _attempts = 0;
        _nextActionMs = now + uint32_t(ESP8266BASE_NTP_SYNC_INTERVAL) * 1000UL;
        ESP8266BASE_LOG_W_P("NTP ", "ntp_verify_aborted reason=%s", reason ? reason : "unknown");
        return;
    }
    if (_attempts < 0xFFU) ++_attempts;
    uint8_t shift = _attempts > 0 ? static_cast<uint8_t>(_attempts - 1) : 0;
    if (shift > 5) shift = 5;  // 2,4,8,16,32,64(封顶 60)
    uint32_t backoff = 2000UL << shift;
    if (backoff > 60000UL) backoff = 60000UL;
    _nextActionMs = now + backoff;
    ESP8266BASE_LOG_W_P("NTP ", "ntp_acquire_rejected reason=%s retry_in=%lums attempt=%u",
                      reason ? reason : "unknown", static_cast<unsigned long>(backoff),
                      static_cast<unsigned>(_attempts));
}

bool Esp8266BaseNTP::_isDue(uint32_t now, uint32_t due) {
    return static_cast<int32_t>(now - due) >= 0;
}

void Esp8266BaseNTP::_finishSync(time_t t) {
    _ntpUdp.stop();
    _waiting = false;
#if ESP8266BASE_USE_JOURNAL
    const uint32_t epoch = static_cast<uint32_t>(t);
    Esp8266BaseJournal::record(JNL_NTP_SYNC, 0, 0,
                               static_cast<uint16_t>(epoch),
                               static_cast<uint16_t>(epoch >> 16U));
#endif
    uint32_t uptimeMs = millis();
    time_t bootTime = t > (time_t)(uptimeMs / 1000UL)
        ? t - (time_t)(uptimeMs / 1000UL)
        : 0;

    char nowBuf[20];
    char bootBuf[20] = "unknown";
    struct tm* tm_info = localtime(&t);
    strftime(nowBuf, sizeof(nowBuf), "%Y-%m-%d %H:%M:%S", tm_info);

    if (bootTime > 0) {
        struct tm* bootTm = localtime(&bootTime);
        strftime(bootBuf, sizeof(bootBuf), "%Y-%m-%d %H:%M:%S", bootTm);
    }

    ESP8266BASE_LOG_I_P("NTP ", "time_synchronized actual_time=%s uptime_ms=%lu boot_time=%s",
                      nowBuf, static_cast<unsigned long>(uptimeMs), bootBuf);

    if (!_logSwitched) {
        _logSwitched = true;
        Esp8266BaseLog::setTimeProvider(_timeStr);
        ESP8266BASE_LOG_I_P("NTP ", "log_timestamp_mode=absolute_datetime");
    }
}

// ----------------------------------------------------------------------------
// 公开查询
// ----------------------------------------------------------------------------
bool Esp8266BaseNTP::isSynced() {
    return _clockTrusted;
}

bool Esp8266BaseNTP::hasMeasuredUncertainty() {
    return _clockTrusted && _uncertaintyMeasured;
}

uint32_t Esp8266BaseNTP::lastSyncRttMs() {
    return hasMeasuredUncertainty() ? _lastSyncRttMs : 0;
}

uint16_t Esp8266BaseNTP::estimatedUncertaintyMs() {
    return hasMeasuredUncertainty() ? _estimatedUncertaintyMs : 0;
}

uint32_t Esp8266BaseNTP::timestamp() {
    const time_t current = time(nullptr);
    return _clockTrusted && current >= 1000000000UL &&
                   static_cast<uint64_t>(current) <= UINT32_MAX
               ? static_cast<uint32_t>(current)
               : 0;
}

bool Esp8266BaseNTP::formatTo(char* out, size_t len, const char* fmt) {
    if (!out || !len || !fmt) return false;
    out[0] = '\0';
    if (!isSynced()) return false;
    time_t t = time(nullptr);
    struct tm* tm_info = localtime(&t);
    return tm_info && strftime(out, len, fmt, tm_info) != 0;
}

const char* Esp8266BaseNTP::_timeStr() {
    static char buf[20];
    if (!isSynced()) {
        snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(millis()));
        return buf;
    }
    time_t t = time(nullptr);
    struct tm* tm_info = localtime(&t);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
    return buf;
}
#endif
