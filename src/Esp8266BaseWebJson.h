#pragma once
#include <stddef.h>
#include <stdint.h>

namespace Esp8266BaseInternal {
// Private flat-object encoder for the Base's health/hostname responses.
// One <=100B stack chunk; no JSON document, heap, global buffer or destructor IO.
class WebJsonWriter {
public:
    using Sink = bool (*)(const uint8_t*, size_t, void*);
    WebJsonWriter(Sink sink, void* context) : sink_(sink), context_(context), ok_(sink != nullptr) {
        chunk_[0] = '{';
    }
    void text(const char* name, const char* value) { key(name); quoted(value); }
    void number(const char* name, uint32_t value) { key(name); digits(value); }
    void signedNumber(const char* name, int32_t value) {
        key(name);
        if (value < 0) byte('-');
        digits(value < 0 ? uint32_t(0) - uint32_t(value) : uint32_t(value));
    }
    void boolean(const char* name, bool value) { key(name); literal(value ? "true" : "false"); }
    bool finish() {
        if (finished_) return false;
        byte('}');
        finished_ = true;
        return flush();
    }
private:
    bool flush() {
        if (ok_ && used_) ok_ = sink_(reinterpret_cast<const uint8_t*>(chunk_), used_, context_);
        used_ = 0;
        return ok_;
    }
    void byte(char value) {
        if (!ok_) return;
        if (used_ == sizeof(chunk_) && !flush()) return;
        chunk_[used_++] = value;
    }
    void literal(const char* value) { while (ok_ && value && *value) byte(*value++); }
    void key(const char* name) {
        if (finished_ || !name) { ok_ = false; return; }
        if (!first_) byte(',');
        first_ = false;
        quoted(name);
        byte(':');
    }
    void quoted(const char* value) {
        byte('"');
        while (ok_ && value && *value) {
            const uint8_t c = static_cast<uint8_t>(*value++);
            if (c == '"' || c == '\\') { byte('\\'); byte(char(c)); }
            else if (c < 0x20) {
                literal("\\u00");
                const uint8_t high = c >> 4, low = c & 15;
                byte(char(high < 10 ? '0' + high : 'a' + high - 10));
                byte(char(low < 10 ? '0' + low : 'a' + low - 10));
            } else byte(char(c));
        }
        byte('"');
    }
    void digits(uint32_t value) {
        char reversed[10];
        size_t count = 0;
        do { reversed[count++] = char('0' + value % 10); value /= 10; } while (value);
        while (count) byte(reversed[--count]);
    }
    Sink sink_;
    void* context_;
    char chunk_[96];
    size_t used_ = 1;
    bool ok_, first_ = true, finished_ = false;
};
#if UINTPTR_MAX == UINT32_MAX
static_assert(sizeof(WebJsonWriter) <= 112, "Web JSON target stack budget");
#endif
} // namespace Esp8266BaseInternal
