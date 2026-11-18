// A path as the system's file service takes it.
#pragma once

#include <switch.h>

#include <algorithm>
#include <cstring>

namespace hbp {

/// libnx sends a path to the file service as FS_MAX_PATH (0x301) bytes from wherever it starts, however short it is, and the kernel turns
/// the request down (2001-0106) unless the app can read all of them. A path in a string on the heap, just below memory that can't be read at
/// that moment (such as where a thread's stack is mapped from), was turned down for it, and each request turned down is a chance of fs.mitm
/// taking the console down (see askCard in nx_sys.cpp). libnx's own stdio copies a path into a buffer this size first, and so does this. One
/// that's too long keeps no end, so that the file service turns it down as before, rather than being sent a shorter path to something else.
/// Made where it's passed, so that it lasts as long as the call: a pointer kept from it is left pointing at nothing.
struct FsPath {
    char text[FS_MAX_PATH] = {};
    explicit FsPath(const char* path) { memcpy(text, path, std::min(strlen(path) + 1, sizeof(text))); }
    operator const char*() const { return text; }
};

}  // namespace hbp
