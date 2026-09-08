#pragma once

#include <stdint.h>
#include <string.h>

namespace Esp8266BaseNTPInternal {

// NTP era unfolding for the supported 1970..2104 window (RFC 4330 section 3).
// A completely zero timestamp is the protocol's unavailable-time sentinel.
inline uint64_t timestampMicros(const uint8_t* value) {
    const uint32_t seconds = (uint32_t(value[0]) << 24) | (uint32_t(value[1]) << 16) |
                             (uint32_t(value[2]) << 8) | uint32_t(value[3]);
    const uint32_t fraction = (uint32_t(value[4]) << 24) | (uint32_t(value[5]) << 16) |
                              (uint32_t(value[6]) << 8) | uint32_t(value[7]);
    if ((seconds == 0 && fraction == 0) ||
        ((seconds & 0x80000000UL) && seconds < 2208988800UL)) return 0;
    const uint32_t unixSeconds = seconds - 2208988800UL;
    return uint64_t(unixSeconds) * 1000000ULL + ((uint64_t(fraction) * 1000000ULL) >> 32);
}

inline bool matchesRequest(const uint8_t* packet, const uint8_t* transmitNonce) {
    return memcmp(packet + 24, transmitNonce, 8) == 0;
}

}
