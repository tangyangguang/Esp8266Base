// Host-only unit tests for the NTP trust-decision pure core.
// Compiled and executed by tools/check_logic.py (clang++/g++); never linked into firmware.
#include "../src/Esp8266BaseNtpCore.h"

#include <cstdio>
#include <cstring>

using Esp8266BaseNtpCore::Sample;
using Esp8266BaseNtpCore::Verdict;

static int g_failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

static constexpr uint32_t T_SEC = 1767225600U;  // 2026-01-01T00:00:00Z
static constexpr uint32_t IP1 = 0x0A000001U;
static constexpr uint32_t IP2 = 0x0A000002U;

static void test_posix_tz() {
    char buf[16];
    Esp8266BaseNtpCore::formatPosixTz(buf, sizeof(buf), 28800);
    CHECK(std::strcmp(buf, "UTC-8") == 0);             // UTC+8
    Esp8266BaseNtpCore::formatPosixTz(buf, sizeof(buf), 19800);
    CHECK(std::strcmp(buf, "UTC-5:30") == 0);          // UTC+5:30
    Esp8266BaseNtpCore::formatPosixTz(buf, sizeof(buf), -18000);
    CHECK(std::strcmp(buf, "UTC+5") == 0);             // UTC-5
    Esp8266BaseNtpCore::formatPosixTz(buf, sizeof(buf), 0);
    CHECK(std::strcmp(buf, "UTC+0") == 0);
}

static void test_agree_window() {
    using Esp8266BaseNtpCore::agreeWindowMs;
    CHECK(agreeWindowMs(0, 0) == 250);
    CHECK(agreeWindowMs(500, 500) == 350);   // 125+125+100
    CHECK(agreeWindowMs(1000, 1000) == 600); // 250+100+250
}

static void test_plausible_window() {
    using Esp8266BaseNtpCore::plausibleUnixEpochSec;
    CHECK(plausibleUnixEpochSec(T_SEC));
    CHECK(!plausibleUnixEpochSec(100U));            // 1970 garbage
    CHECK(!plausibleUnixEpochSec(0xFFFFFFFFU));     // 2106 garbage
}

static Sample make(uint32_t ip, uint32_t atMs, uint32_t sec, uint16_t ms, uint16_t rtt) {
    return Sample{ip, atMs, sec, ms, rtt};
}

static void test_acquire_trusted() {
    // a 在 1000ms 估 ...100ms，b 在 1050ms 估 ...150ms：归一后一致。
    Sample a = make(IP1, 1000, T_SEC, 100, 50);
    Sample b = make(IP2, 1050, T_SEC, 150, 60);
    uint64_t trusted = 0;
    CHECK(Esp8266BaseNtpCore::evaluateAcquisition(a, b, trusted) == Verdict::TRUSTED);
    // trusted 是 b 时刻的可信时间（...150ms）
    CHECK(trusted == uint64_t(T_SEC) * 1000000ULL + 150000ULL);
}

static void test_acquire_one_source_10s_fast() {
    Sample a = make(IP1, 1000, T_SEC, 100, 50);
    Sample b = make(IP2, 1050, T_SEC + 10, 150, 60);  // b 快 10s
    uint64_t trusted = 999;
    CHECK(Esp8266BaseNtpCore::evaluateAcquisition(a, b, trusted) == Verdict::UNTRUSTED);
    CHECK(trusted == 0);
}

static void test_acquire_same_ip_rejected() {
    Sample a = make(IP1, 1000, T_SEC, 100, 50);
    Sample b = make(IP1, 1050, T_SEC, 150, 60);
    uint64_t trusted = 0;
    CHECK(Esp8266BaseNtpCore::evaluateAcquisition(a, b, trusted) == Verdict::UNTRUSTED);
}

static void test_acquire_high_rtt_rejected() {
    Sample a = make(IP1, 1000, T_SEC, 100, 50);
    Sample b = make(IP2, 1050, T_SEC, 150, 700);  // > 500ms gate
    uint64_t trusted = 0;
    CHECK(Esp8266BaseNtpCore::evaluateAcquisition(a, b, trusted) == Verdict::UNTRUSTED);
}

static void test_acquire_implausible_rejected() {
    Sample a = make(IP1, 1000, T_SEC, 100, 50);
    Sample b = make(IP2, 1050, 100U, 150, 60);
    uint64_t trusted = 0;
    CHECK(Esp8266BaseNtpCore::evaluateAcquisition(a, b, trusted) == Verdict::UNTRUSTED);
}

static void test_acquire_incomplete() {
    Sample a = make(IP1, 1000, T_SEC, 100, 50);
    Sample b{};  // 未收到
    uint64_t trusted = 0;
    CHECK(Esp8266BaseNtpCore::evaluateAcquisition(a, b, trusted) == Verdict::INCOMPLETE);
}

static void test_acquire_millis_wrap() {
    // a 在 0xFFFFFFF0，b 在回绕后 0x10（晚 32ms）；b 估计比 a 多 32ms。
    Sample a = make(IP1, 0xFFFFFFF0U, T_SEC, 0, 40);
    Sample b = make(IP2, 0x10U, T_SEC, 32, 40);
    uint64_t trusted = 0;
    CHECK(Esp8266BaseNtpCore::evaluateAcquisition(a, b, trusted) == Verdict::TRUSTED);
}

static void test_verify_clock_accurate() {
    Sample a = make(IP1, 1000, T_SEC, 100, 50);
    Sample b = make(IP2, 1050, T_SEC, 150, 60);
    const uint64_t trustedAtB = uint64_t(T_SEC) * 1000000ULL + 150000ULL;
    uint64_t trustedNow = 0;
    CHECK(Esp8266BaseNtpCore::evaluateVerification(a, b, trustedAtB, 1050, trustedNow) ==
          Verdict::TRUSTED);
}

static void test_verify_local_clock_10s_fast_is_suspect() {
    Sample a = make(IP1, 1000, T_SEC, 100, 50);
    Sample b = make(IP2, 1050, T_SEC, 150, 60);
    const uint64_t trustedAtB = uint64_t(T_SEC) * 1000000ULL + 150000ULL;
    const uint64_t localFast = trustedAtB + 10000000ULL;  // 本地墙钟快 10s
    uint64_t trustedNow = 0;
    CHECK(Esp8266BaseNtpCore::evaluateVerification(a, b, localFast, 1050, trustedNow) ==
          Verdict::SUSPECT);
}

static void test_verify_sources_disagree_inconclusive() {
    Sample a = make(IP1, 1000, T_SEC, 100, 50);
    Sample b = make(IP2, 1050, T_SEC + 30, 150, 60);  // 双源相差 30s
    uint64_t trustedNow = 0;
    CHECK(Esp8266BaseNtpCore::evaluateVerification(a, b, 0, 1050, trustedNow) ==
          Verdict::INCOMPLETE);
}

int main() {
    test_posix_tz();
    test_agree_window();
    test_plausible_window();
    test_acquire_trusted();
    test_acquire_one_source_10s_fast();
    test_acquire_same_ip_rejected();
    test_acquire_high_rtt_rejected();
    test_acquire_implausible_rejected();
    test_acquire_incomplete();
    test_acquire_millis_wrap();
    test_verify_clock_accurate();
    test_verify_local_clock_10s_fast_is_suspect();
    test_verify_sources_disagree_inconclusive();
    if (g_failures) {
        std::printf("ntp core tests: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("ntp core tests: all passed\n");
    return 0;
}
