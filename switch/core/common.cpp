#include "common.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>

#include "sys.h"

namespace hbp {

namespace {
JobSink g_quiet;
JobSink* g_sink = &g_quiet;
constexpr double ProgressInterval = 0.1;
}  // namespace

JobSink& sink() { return *g_sink; }
void setSink(JobSink* value) { g_sink = value ? value : &g_quiet; }

std::string format(const char* text, ...) {
    va_list args;
    va_start(args, text);
    va_list again;
    va_copy(again, args);
    int size = vsnprintf(nullptr, 0, text, args);
    va_end(args);
    std::string out(size > 0 ? (size_t)size : 0, '\0');
    if (size > 0)
        vsnprintf(&out[0], (size_t)size + 1, text, again);
    va_end(again);
    return out;
}

std::string toHex(const void* data, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string out(size * 2, '0');
    const u8* b = (const u8*)data;
    for (size_t i = 0; i < size; i++) {
        out[i * 2] = digits[b[i] >> 4];
        out[i * 2 + 1] = digits[b[i] & 15];
    }
    return out;
}

static int digitValue(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool fromHex(const std::string& text, Bytes& out) {
    if (text.size() % 2 != 0)
        return false;
    out.resize(text.size() / 2);
    for (size_t i = 0; i < out.size(); i++) {
        int high = digitValue(text[i * 2]), low = digitValue(text[i * 2 + 1]);
        if (high < 0 || low < 0)
            return false;
        out[i] = (u8)(high << 4 | low);
    }
    return true;
}

std::string formatSize(i64 bytes) {
    const char* sign = bytes < 0 ? "-" : "";
    double value = (double)(bytes < 0 ? -bytes : bytes);
    if (value >= 1024.0 * 1024 * 1024)
        return format("%s%.2f GB", sign, value / (1024.0 * 1024 * 1024));
    if (value >= 1024.0 * 1024)
        return format("%s%.1f MB", sign, value / (1024.0 * 1024));
    if (value >= 1024.0)
        return format("%s%.1f KB", sign, value / 1024.0);
    return format("%s%lld B", sign, (long long)value);
}

std::string formatElapsed(double seconds) {
    if (seconds < 90)
        return format("%.0f s", seconds);
    return format("%.1f min", seconds / 60);
}

bool endsWithNoCase(const std::string& text, const char* suffix) {
    size_t n = strlen(suffix);
    if (text.size() < n)
        return false;
    for (size_t i = 0; i < n; i++) {
        if (tolower((u8)text[text.size() - n + i]) != tolower((u8)suffix[i]))
            return false;
    }
    return true;
}

bool equalsNoCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (tolower((u8)a[i]) != tolower((u8)b[i]))
            return false;
    }
    return true;
}

bool containsNoCase(const std::string& text, const std::string& part) {
    if (part.size() > text.size())
        return false;
    for (size_t i = 0; i + part.size() <= text.size(); i++) {
        size_t j = 0;
        while (j < part.size() && tolower((u8)text[i + j]) == tolower((u8)part[j]))
            j++;
        if (j == part.size())
            return true;
    }
    return false;
}

void Progress::report(i64 done, i64 output) {
    checkStop();
    double now = sys::now();
    if (_last >= 0 && now - _last < ProgressInterval)
        return;
    _last = now;
    sink().progress(_label, done, _total, output);
}

void Progress::done() { sink().progress(_label, _total, _total, 0); }

}  // namespace hbp
