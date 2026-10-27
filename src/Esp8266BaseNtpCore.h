#pragma once

#include <stdint.h>
#include <stdio.h>

// ----------------------------------------------------------------------------
// Esp8266BaseNtpCore — NTP 信任判定纯核（零 Arduino 依赖，可在 host 上单测）
//
// 设计不变量：系统墙钟只能在启动期、联网放行前，由两个【不同 IP】的钉死权威
// 时间源在有界往返延迟内、归一到同一单调时刻后结果一致时设置一次。任何单源、
// 同源、高延迟或时间值不合理的结果都不得被采用。运行中只校验、不改钟。
// ----------------------------------------------------------------------------

namespace Esp8266BaseNtpCore {

// 合理 Unix 时间窗（钉死，拒绝 1900/2100 等垃圾值）：2023-01-01 .. 2035-01-01。
constexpr uint32_t MIN_PLAUSIBLE_UNIX_SEC = 1672531200U;
constexpr uint32_t MAX_PLAUSIBLE_UNIX_SEC = 2051222400U;

// 单个时间源的往返质量门限：超过该 RTT 的样本不参与信任判定。
constexpr uint32_t RTT_GATE_MS = 500;
// 两源一致窗：在与实测 RTT 挂钩的基础上设下限，避免弱网误拒又不放任大偏差。
constexpr uint32_t MIN_AGREE_MS = 250;
constexpr uint32_t AGREE_MARGIN_MS = 100;
// 运行中校验：双源一致时间与本地墙钟偏差超过该值才标记可疑（绝不改钟）。
constexpr uint32_t SUSPECT_MS = 2000;

// 单个时间源样本（紧凑 16 字节，两个常驻样本仅 32B）。墙钟设置只需毫秒精度。
struct Sample {
    uint32_t serverIp4 = 0;   // 解析到的 IPv4（主机序数值），0 兼作无效标记
    uint32_t atMs = 0;        // 接收时刻的单调时钟毫秒
    uint32_t estSec = 0;      // 接收时刻对真值估计的 Unix 秒（0=无效样本）
    uint16_t estMs = 0;       // 估计的亚秒毫秒（0..999）
    uint16_t rttMs = 0;       // 往返毫秒（已排除 DNS）

    bool valid() const { return estSec != 0; }
    uint64_t estimateUs() const {
        return uint64_t(estSec) * 1000000ULL + uint64_t(estMs) * 1000ULL;
    }
};

enum class Verdict : uint8_t {
    INCOMPLETE = 0,  // 样本未齐或网络不可得：等待/重试，不做任何信任结论
    UNTRUSTED = 1,   // 样本齐但不满足信任条件：丢弃整批并重新采集
    TRUSTED = 2,     // 双源一致可信
    SUSPECT = 3,     // 仅运行中校验：双源彼此一致但与本地墙钟显著不符
};

inline bool plausibleUnixEpochSec(uint32_t sec) {
    return sec >= MIN_PLAUSIBLE_UNIX_SEC && sec <= MAX_PLAUSIBLE_UNIX_SEC;
}

// 与实测 RTT 挂钩的两源一致窗（毫秒）。非对称路由下单程偏差量级≈RTT/4。
inline uint32_t agreeWindowMs(uint32_t rttA, uint32_t rttB) {
    uint32_t v = (rttA / 4) + (rttB / 4) + AGREE_MARGIN_MS;
    return v < MIN_AGREE_MS ? MIN_AGREE_MS : v;
}

// 把较早样本的估计按本地单调钟推进折算到较晚接收时刻。调用方须保证后者不早。
inline uint64_t normalizeToLaterUs(uint64_t estimateUsA, uint32_t atMsA, uint32_t atMsB) {
    return estimateUsA + uint64_t(atMsB - atMsA) * 1000ULL;
}

// 启动期：依据两个源的样本判定是否可信设钟。可信时 trustedUsAtB 给出在样本 b
// 接收时刻的可信时间（微秒），调用方设置墙钟时还应加上此后流逝。
inline Verdict evaluateAcquisition(const Sample& a, const Sample& b,
                                   uint64_t& trustedUsAtB) {
    trustedUsAtB = 0;
    if (!a.valid() || !b.valid()) return Verdict::INCOMPLETE;
    // 两个域名解析到同一 IP 不构成独立确认（可能是同一台被 DHCP/DNS 劫持的设备）。
    if (a.serverIp4 == 0 || b.serverIp4 == 0 || a.serverIp4 == b.serverIp4)
        return Verdict::UNTRUSTED;
    if (a.rttMs > RTT_GATE_MS || b.rttMs > RTT_GATE_MS) return Verdict::UNTRUSTED;
    if (!plausibleUnixEpochSec(a.estSec) || !plausibleUnixEpochSec(b.estSec))
        return Verdict::UNTRUSTED;

    const uint64_t aEst = a.estimateUs();
    const uint64_t bEst = b.estimateUs();
    const uint64_t aAtB = normalizeToLaterUs(aEst, a.atMs, b.atMs);
    const uint64_t diffUs = aAtB >= bEst ? aAtB - bEst : bEst - aAtB;
    if (diffUs > uint64_t(agreeWindowMs(a.rttMs, b.rttMs)) * 1000ULL)
        return Verdict::UNTRUSTED;

    // 两源在 b 时刻一致：取均值降低单源抖动（无 64 位溢出写法）。
    trustedUsAtB = (aAtB / 2) + (bEst / 2) + ((aAtB & 1U) & (bEst & 1U));
    return Verdict::TRUSTED;
}

// 运行中只校验：双源必须先彼此一致可信，再比较其与本地墙钟（localNowUs）。
// 网络不可得/样本不齐返回 INCOMPLETE（不改变可疑标志）；双源一致但本地钟偏差
// 超过 SUSPECT_MS 返回 SUSPECT；本地钟仍准确返回 TRUSTED。
inline Verdict evaluateVerification(const Sample& a, const Sample& b,
                                    uint64_t localNowUs, uint32_t localNowMs,
                                    uint64_t& trustedUsNow) {
    uint64_t trustedAtB = 0;
    const Verdict acq = evaluateAcquisition(a, b, trustedAtB);
    if (acq != Verdict::TRUSTED) return Verdict::INCOMPLETE;

    trustedUsNow = trustedAtB + uint64_t(localNowMs - b.atMs) * 1000ULL;
    if (!plausibleUnixEpochSec(uint32_t(localNowUs / 1000000ULL)))
        return Verdict::SUSPECT;
    const uint64_t diffUs = trustedUsNow >= localNowUs ? trustedUsNow - localNowUs
                                                       : localNowUs - trustedUsNow;
    return diffUs > uint64_t(SUSPECT_MS) * 1000ULL ? Verdict::SUSPECT : Verdict::TRUSTED;
}

// 把“相对 UTC 的整数秒偏移”（UTC+8=28800）转成 POSIX TZ 串（反号：UTC+8 -> "UTC-8"），
// 支持半小时与负偏移。调用方需提供至少 10 字节缓冲。
inline void formatPosixTz(char* out, size_t len, int timezoneOffsetSec) {
    if (out == nullptr || len < 10) return;
    const long posix = -long(timezoneOffsetSec);  // POSIX 规则与习惯相反
    const long absv = posix < 0 ? -posix : posix;
    const long hh = absv / 3600;
    const long mm = (absv % 3600) / 60;
    const char sign = posix < 0 ? '-' : '+';
    if (mm != 0)
        snprintf(out, len, "UTC%c%ld:%02ld", sign, hh, mm);
    else
        snprintf(out, len, "UTC%c%ld", sign, hh);
}

}  // namespace Esp8266BaseNtpCore
