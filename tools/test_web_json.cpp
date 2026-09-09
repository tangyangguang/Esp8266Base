#include <cassert>
#include <climits>
#include <string>
#include "../src/Esp8266BaseWebJson.h"

using Esp8266BaseInternal::WebJsonWriter;
static_assert(sizeof(WebJsonWriter) <= 128, "bounded writer state on host and target");
#if UINTPTR_MAX == UINT32_MAX
static_assert(sizeof(WebJsonWriter) <= 112, "target stack budget");
#endif

struct Output {
    std::string bytes;
    size_t calls = 0, failAt = 0;
    static bool sink(const uint8_t* data, size_t length, void* context) {
        auto& output = *static_cast<Output*>(context);
        assert(length > 0 && length <= 96);
        if (++output.calls == output.failAt) return false;
        output.bytes.append(reinterpret_cast<const char*>(data), length);
        return true;
    }
};

int main() {
    Output output;
    {
        WebJsonWriter json(Output::sink, &output);
        json.text("key\"", "a\"b\\c\x01\n");
        json.number("u", UINT32_MAX);
        json.signedNumber("s", INT32_MIN);
        json.boolean("t", true);
        json.boolean("f", false);
        json.text("empty", nullptr);
        assert(json.finish());
        assert(!json.finish());
        const size_t calls = output.calls;
        json.text("late", "must not emit");
        assert(!json.finish() && output.calls == calls);
    }
    assert(output.bytes == "{\"key\\\"\":\"a\\\"b\\\\c\\u0001\\u000a\",\"u\":4294967295,"
                           "\"s\":-2147483648,\"t\":true,\"f\":false,\"empty\":\"\"}");
    output = Output{};
    {
        const std::string hostname(32, 'h');
        WebJsonWriter json(Output::sink, &output);
        json.text("hostname", hostname.c_str());
        json.text("persisted", hostname.c_str());
        json.text("default", hostname.c_str());
        json.boolean("rebootRequired", false);
        assert(json.finish());
        assert(output.bytes == "{\"hostname\":\"" + hostname + "\",\"persisted\":\"" + hostname +
                               "\",\"default\":\"" + hostname + "\",\"rebootRequired\":false}");
        assert(output.bytes.size() > 160); // The old fixed snprintf buffer truncated this.
    }
    for (size_t length = 0; length <= 300; ++length) {
        output = Output{};
        const std::string value(length, 'v');
        WebJsonWriter json(Output::sink, &output);
        json.text("field", value.c_str());
        json.number("zero", 0);
        json.signedNumber("positive", INT32_MAX);
        json.signedNumber("zeroSigned", 0);
        assert(json.finish());
        assert(output.bytes == "{\"field\":\"" + value + "\",\"zero\":0,"
                               "\"positive\":2147483647,\"zeroSigned\":0}");
        assert(output.calls == (output.bytes.size() + 95) / 96);
    }
    output = Output{};
    {
        WebJsonWriter json(Output::sink, &output);
        json.text("utf8", "\xc3\xa9\xe4\xb8\xad");
        assert(json.finish());
        assert(output.bytes == "{\"utf8\":\"\xc3\xa9\xe4\xb8\xad\"}");
    }
    output = Output{};
    const std::string controls(200, '\x01');
    {
        WebJsonWriter json(Output::sink, &output);
        json.text("control", controls.c_str());
        assert(json.finish());
        std::string expected = "{\"control\":\"";
        for (unsigned n = 0; n < 200; ++n) expected += "\\u0001";
        assert(output.bytes == expected + "\"}");
    }
    const size_t chunks = output.calls;
    for (size_t failAt = 1; failAt <= chunks; ++failAt) {
        output = Output{};
        output.failAt = failAt;
        WebJsonWriter json(Output::sink, &output);
        json.text("control", controls.c_str());
        json.boolean("later", true);
        assert(!json.finish());
        assert(output.calls == failAt); // No retry or later write after sink failure.
    }
    WebJsonWriter missing(nullptr, nullptr);
    missing.text("unused", "value");
    assert(!missing.finish());
    output = Output{};
    {
        WebJsonWriter json(Output::sink, &output);
        assert(json.finish() && output.bytes == "{}");
    }
    output = Output{};
    {
        WebJsonWriter json(Output::sink, &output);
        json.text(nullptr, "invalid");
        assert(!json.finish() && output.calls == 0);
    }
}
