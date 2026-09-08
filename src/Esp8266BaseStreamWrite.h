#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Esp8266BaseInternal {

// Fixed-memory complete write. Progress never restarts the caller's budget.
// A blocking SDK write can only be checked after it returns.
template <typename Client, typename Clock, typename Cooperate>
bool writeAll(Client& client, const uint8_t* bytes, size_t length,
              uint32_t startedMs, uint32_t timeoutMs, Clock clock, Cooperate cooperate) {
    if (length && !bytes) return false;
    size_t offset = 0;
    while (offset < length) {
        if (!client.connected() || clock() - startedMs >= timeoutMs) return false;
        const size_t count = client.write(bytes + offset, length - offset);
        if (count > length - offset || clock() - startedMs >= timeoutMs) return false;
        offset += count;
        cooperate();
    }
    return true;
}

// Never pass zero to SDK flush: zero selects its own default wait budget.
template <typename Client, typename Clock>
bool flushBeforeDeadline(Client& client, uint32_t deadline, Clock clock) {
    const uint32_t remaining = deadline - clock();
    if (!remaining || remaining > 0x7fffffffUL) return false;
    if (!client.flush(remaining)) return false;
    return static_cast<int32_t>(clock() - deadline) < 0;
}

}
