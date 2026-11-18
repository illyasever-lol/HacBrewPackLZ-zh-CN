// What every part of the engine shares. The engine is the C# tool's engine ported to C++, so that the same game gives
// the same bytes on a console as on a PC. It knows nothing of the console or of any screen: see sys.h and console.h.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace hbp {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i32 = int32_t;
using i64 = int64_t;
using Bytes = std::vector<u8>;

/// Something went wrong, in words fit to show to whoever is using this.
struct ToolError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// What was read isn't what it should be: it doesn't match its hashes, a compressed block in it doesn't decode, or an update reading it needs
/// a part of the original game that isn't there. Read through a compressed copy, that's an update that doesn't fit the copy, rather than a
/// file that can't be read.
struct MismatchError : ToolError {
    using ToolError::ToolError;
};

/// Whether size bytes from offset lie within length, without the sum overflowing, as it can with what a damaged file says (Ranges.Fits on the PC).
inline bool fitsWithin(i64 offset, i64 size, i64 length) { return offset >= 0 && size >= 0 && offset <= length && size <= length - offset; }

/// The most a table of names or entries is read whole for. Real ones are a few KB; a damaged one could ask for gigabytes.
constexpr i64 MostTable = 16LL << 20;

/// Whether what went wrong was the system turning a request down (askCard in nx_sys.cpp), which can go through later on.
inline bool refusedBySystem(const std::string& message) {
    return message.find("(2001-0106)") != std::string::npos || message.find("(2001-0105)") != std::string::npos;
}

/// The system says the file system being written to is damaged. Writing more to it can only make that worse, so whatever's being done
/// stops there, rather than going on to the next game.
struct DamagedFileSystem : ToolError {
    using ToolError::ToolError;
};

/// A FAT file system the system has found damaged: the 2002-4681 to 2002-4699 range, as an SD card or the console's own storage can be.
inline bool isDamagedFileSystem(u32 module, u32 description) { return module == 2 && description >= 4681 && description <= 4699; }

/// Whether what went wrong is a damaged file system, whether it came as one or was passed on in other words, which keep the system's code.
inline bool saysFileSystemDamaged(const ToolError& e) {
    if (dynamic_cast<const DamagedFileSystem*>(&e))
        return true;
    std::string text = e.what();
    for (size_t at = text.find("2002-"); at != std::string::npos; at = text.find("2002-", at + 5)) {
        u32 description = 0;
        size_t i = at + 5;
        for (; i < text.size() && i < at + 9 && text[i] >= '0' && text[i] <= '9'; i++)
            description = description * 10 + (u32)(text[i] - '0');
        if (i == at + 9 && isDamagedFileSystem(2, description))
            return true;
    }
    return false;
}

/// The person asked to stop.
struct Cancelled {};

/// The highest set bit of a value that isn't zero.
inline int highestBit(u32 value) {
#ifdef _MSC_VER
    unsigned long index;
    _BitScanReverse(&index, value);
    return (int)index;
#else
    return 31 - __builtin_clz(value);
#endif
}

/// The lowest set bit of a value that isn't zero.
inline int lowestBit(u64 value) {
#ifdef _MSC_VER
    unsigned long index;
    _BitScanForward64(&index, value);
    return (int)index;
#else
    return __builtin_ctzll(value);
#endif
}

inline void prefetch(const void* address) {
#ifdef _MSC_VER
    _mm_prefetch((const char*)address, _MM_HINT_T0);
#else
    __builtin_prefetch(address);
#endif
}

inline u16 rd16(const void* p) { u16 v; memcpy(&v, p, 2); return v; }
inline u32 rd32(const void* p) { u32 v; memcpy(&v, p, 4); return v; }
inline u64 rd64(const void* p) { u64 v; memcpy(&v, p, 8); return v; }
inline i32 rdi32(const void* p) { return (i32)rd32(p); }
inline i64 rdi64(const void* p) { return (i64)rd64(p); }
inline void wr16(void* p, u16 v) { memcpy(p, &v, 2); }
inline void wr32(void* p, u32 v) { memcpy(p, &v, 4); }
inline void wr64(void* p, u64 v) { memcpy(p, &v, 8); }
inline void wrBe64(void* p, u64 v) {
    u8* b = (u8*)p;
    for (int i = 0; i < 8; i++)
        b[i] = (u8)(v >> (56 - i * 8));
}

std::string format(const char* text, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
std::string toHex(const void* data, size_t size);
bool fromHex(const std::string& text, Bytes& out);

/// Sizes the way the rest of the tool shows them: 1 GB is 1024 MB.
std::string formatSize(i64 bytes);
std::string formatElapsed(double seconds);

bool endsWithNoCase(const std::string& text, const char* suffix);
bool equalsNoCase(const std::string& a, const std::string& b);
bool containsNoCase(const std::string& text, const std::string& part);

/// What the engine reports to, and what can stop it. There is one job at a time, so one of these at a time.
class JobSink {
public:
    virtual ~JobSink() = default;
    virtual void message(const std::string&) {}
    virtual void warning(const std::string&) {}
    /// A step of the current title. Output is how much it has come to so far, where that means anything.
    virtual void progress(const char* /*label*/, i64 /*done*/, i64 /*total*/, i64 /*output*/) {}
    /// A step there's no knowing the size of until it's over, so it can only say how far it's got.
    virtual void counting(const char* /*label*/, i64 /*sofar*/) {}
    /// How many files it's got through, out of how many if that's known.
    virtual void files(i64 /*done*/, i64 /*total*/) {}
    std::atomic<bool> stop{false};
};

JobSink& sink();
void setSink(JobSink* sink);

inline void checkStop() {
    if (sink().stop.load(std::memory_order_relaxed))
        throw Cancelled();
}

/// Reports a step of a job at a pace a screen can keep up with.
class Progress {
public:
    Progress(const char* label, i64 total) : _label(label), _total(total) {}
    void report(i64 done, i64 output = 0);
    void done();

private:
    const char* _label;
    i64 _total;
    double _last = -1;
};

}  // namespace hbp
