// The engine's view of the console: the SD card, USB drives, threads and time.
#include <dirent.h>
#include <fcntl.h>
#include <malloc.h>
#include <unwind.h>
#include <switch.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <usbhsfs.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <new>

#include "../core/sys.h"
#include "../core/remote.h"
#include "nx_path.h"

extern "C" char __start__;

extern "C" char* fake_heap_start;
extern "C" char* fake_heap_end;

namespace hbp::sys {

namespace {

/// The main thread has 0x2C, which is what keeps the screen responsive. The kernel only takes turns among threads of the same priority
/// at the lowest ones, so that's where the sums get done, and whatever waits for the card gets ahead of them when it has something to do.
constexpr int WorkPriority = 0x3B;
constexpr int WaitingPriority = 0x2D;

std::string codeOf(Result rc) { return format("%04u-%04u", 2000 + R_MODULE(rc), R_DESCRIPTION(rc)); }

/// What went wrong, as the error it is: a file system the system says is damaged stops whatever is being done.
[[noreturn]] void fail(Result rc, const std::string& message) {
    if (isDamagedFileSystem(R_MODULE(rc), R_DESCRIPTION(rc)))
        throw DamagedFileSystem(message);
    throw ToolError(message);
}

FsFileSystem* sdCard() {
    FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc");
    if (!fs)
        throw ToolError("无法访问 SD 卡");
    return fs;
}

/// The SD card gets used directly, as the file system it comes from is quick to work with and gives real error codes. Anything else is a USB drive.
bool onCard(const std::string& path) { return path.compare(0, 5, "sdmc:") == 0; }

/// The file system doesn't want to hear the SD card's name, and takes the path from a buffer of its own size (see FsPath).
FsPath inner(const std::string& path) { return FsPath(onCard(path) ? path.c_str() + 5 : path.c_str()); }

constexpr int SystemCore = 3;

std::atomic<bool> g_inForeground{true};

bool canStartThreadOn(int core) {
    ::Thread thread;
    if (R_FAILED(threadCreate(&thread, [](void*) {}, nullptr, nullptr, 0x10000, WorkPriority, core)))
        return false;
    bool started = R_SUCCEEDED(threadStart(&thread));
    if (started)
        threadWaitForExit(&thread);
    threadClose(&thread);
    return started;
}

/// The last core is the system's, and what's left over of it is only for the taking when this was opened as a game. It's what the home menu runs on,
/// which gets it whenever it's wanted, as nothing here is ever ahead of the system. Even so, whatever's on it stops while something else is on the screen.
u64 allowedCores() {
    static const u64 cores = [] {
        u64 mask = 0;
        svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
        mask &= 15;
        AppletType type = appletGetAppletType();
        bool openedAsGame = type == AppletType_Application || type == AppletType_SystemApplication;
        if ((mask >> SystemCore & 1) && !(openedAsGame && canStartThreadOn(SystemCore)))
            mask &= ~(1ull << SystemCore);
        return mask ? mask : 1;
    }();
    return cores;
}

}  // namespace

double now() { return (double)armTicksToNs(armGetSystemTick()) / 1e9; }

void sleepMs(int milliseconds) { svcSleepThread((s64)milliseconds * 1000000); }

void giveWay(size_t bytes) {
    // Which is about what it takes for the home menu not to notice.
    constexpr size_t BytesPerMillisecond = 24 << 10;
    constexpr int AtLeast = 2;
    bool foreground = g_inForeground;
    fsSetPriority(foreground ? FsPriority_Normal : FsPriority_Background);
    if (!foreground)
        sleepMs(AtLeast + (int)(bytes / BytesPerMillisecond));
}

void randomBytes(void* out, size_t size) { randomGet(out, size); }

struct File::Impl {
    std::shared_ptr<remote::RemoteFile> remote;
    FsFile file;
    std::string path;
    bool usb = false;
    int descriptor = -1;
    /// A USB drive has one position in a file to read and write at, so only one thread at a time gets to.
    ::Mutex position = {};
};

std::atomic<i64> g_refusals[2];
/// What the last request turned down was for, for the log.
::Mutex g_lastRefusal = {};
std::string g_lastRefusedFor;

static bool isRefusal(Result rc) {
    return rc == MAKERESULT(Module_Kernel, KernelError_OutOfHandles) || rc == MAKERESULT(Module_Kernel, KernelError_InvalidMemoryState);
}

/// "Path not found" (2002-0001: the file system's module is 2, which libnx has no name for), the one answer that means nothing's there. Any
/// other error says nothing about it.
static bool isNotFound(Result rc) { return rc == MAKERESULT(2, 1); }

/// The same for a file or folder on a USB drive, from errno.
static bool isMissingErrno(int error) { return error == ENOENT || error == ENOTDIR; }

/// What the app has asked of the card and not had answered yet, for the diagnosis below.
static std::atomic<int> g_inFlight{0};
struct InFlight {
    InFlight() { g_inFlight++; }
    ~InFlight() { g_inFlight--; }
};

/// Why the card turns requests down isn't known, and each one has been seen to put custom firmware's fs.mitm at risk of taking the console
/// down. The kernel answers 2001-0106 when what it's given to read for a request isn't as it should be, so the first few each time the app
/// opens are written down in detail for the log: what was asked, how many other requests of the app's were going, and the state of the
/// memory it was asked from. Only reading, as svcQueryMemory is.
static ::Mutex g_detailsLock = {};
static std::vector<std::string> g_details;
static int g_detailed = 0;

static std::string memoryAt(const char* name, u64 address) {
    MemoryInfo info = {};
    u32 page = 0;
    if (R_FAILED(svcQueryMemory(&info, &page, address)))
        return format("%s %#llx can't be told", name, (unsigned long long)address);
    return format("%s %#llx in %#llx+%#llx type %#x attr %#x perm %#x ipc %u device %u", name, (unsigned long long)address, (unsigned long long)info.addr,
                  (unsigned long long)info.size, info.type, info.attr, info.perm, info.ipc_refcount, info.device_refcount);
}

/// The memory a request was just made from: the stack the thread is at, which the path it sent was copied to (FsPath), a little further down
/// it, the thread's own block the request itself is written in, and the request's buffer, when it had one.
static std::string memoryNow(const void* buffer = nullptr, size_t size = 0) {
    volatile u8 here = 0;
    u64 stack = (u64)(uintptr_t)&here;
    std::string said = memoryAt("stack", stack) + "; " + memoryAt("below", stack - 0x800) + "; " + memoryAt("request", (u64)(uintptr_t)armGetTls());
    if (buffer && size > 0) {
        said += "; " + memoryAt("buffer", (u64)(uintptr_t)buffer);
        if (size > 1)
            said += "; " + memoryAt("its end", (u64)(uintptr_t)buffer + size - 1);
    }
    return said;
}

/// Noted with the memory it was asked from, which is only looked at while there's still to be a note of it.
static void noteRefusal(const char* asked, const std::string& what, Result rc, int others, const void* buffer = nullptr, size_t size = 0) {
    constexpr int MostDetailed = 40;
    mutexLock(&g_detailsLock);
    if (g_detailed < MostDetailed) {
        g_detailed++;
        u64 thread = 0;
        svcGetThreadId(&thread, CUR_THREAD_HANDLE);
        g_details.push_back(format("Turned down (%s): %s %s (%d characters) at %.2f s, %d other requests of the app's going then, thread %llu; %s",
                                   codeOf(rc).c_str(), asked, what.c_str(), (int)what.size(), now(), others, (unsigned long long)thread,
                                   memoryNow(buffer, size).c_str()));
    } else if (g_detailed == MostDetailed) {
        g_detailed++;
        g_details.push_back(format("More turned down after these %d aren't written down one by one, only counted", MostDetailed));
    }
    mutexUnlock(&g_detailsLock);
}

/// Requests to the card have been seen to be turned down by the kernel at no particular time, even when hardly anything else was going on, with
/// 2001-0106. That's an invalid memory state, not being out of handles, which is 2001-0105, though it was long taken for that. It's one file or
/// folder that's turned down, for anything from a second to more than five, while everything around it goes through. Each one is a chance of
/// custom firmware's fs.mitm taking the console down: it waits again for the app's next request, and a cancel its own threads leave behind ends
/// that wait in a way it doesn't expect, so it aborts. Asking again at once was turned down every time it was looked at, so each is asked once:
/// what's turned down is never taken to be missing (presence), and is tried again later where that's worth it. Each is counted, and noted for the
/// log.
template <typename Attempt>
Result askCard(const std::string& what, const char* asked, Attempt attempt) {
    int others = 0;
    Result rc;
    {
        InFlight going;
        others = g_inFlight - 1;
        rc = attempt();
    }
    if (!isRefusal(rc))
        return rc;
    g_refusals[rc == MAKERESULT(Module_Kernel, KernelError_OutOfHandles) ? 0 : 1]++;
    noteRefusal(asked, what, rc, others);
    mutexLock(&g_lastRefusal);
    g_lastRefusedFor = what;
    mutexUnlock(&g_lastRefusal);
    return rc;
}

std::vector<std::string> takeRefusalDetails() {
    mutexLock(&g_detailsLock);
    std::vector<std::string> taken;
    taken.swap(g_details);
    mutexUnlock(&g_detailsLock);
    return taken;
}

Refusals refusals() {
    Refusals refused;
    refused.outOfHandles = g_refusals[0];
    refused.memoryState = g_refusals[1];
    mutexLock(&g_lastRefusal);
    refused.lastFor = g_lastRefusedFor;
    mutexUnlock(&g_lastRefusal);
    return refused;
}

File::File(const std::string& path, Mode mode, i64 sizeHint) {
    if (remote::isRemote(path)) {
        if (mode != Mode::Read)
            throw ToolError("PC 上的文件只能读取");
        std::unique_ptr<Impl> impl(new Impl);
        impl->path = path;
        impl->remote = remote::open(path);
        _impl = impl.release();
        return;
    }
    // Only a file of 4 GB or more gets to be a concatenation file, which a FAT32 card can't do without but which a PC shows as a folder.
    constexpr i64 LargestPlainFile = 0xFFFF0000LL;
    if (!onCard(path)) {
        std::unique_ptr<Impl> impl(new Impl);
        impl->path = path;
        impl->usb = true;
        mutexInit(&impl->position);
        int flags = mode == Mode::Read ? O_RDONLY : mode == Mode::Create || mode == Mode::Sized ? O_RDWR | O_CREAT | O_TRUNC : O_RDWR;
        impl->descriptor = open(path.c_str(), flags, 0666);
        if (impl->descriptor < 0)
            throw ToolError(format("Couldn't open '%s' (%s)", path.c_str(), strerror(errno)));
        _impl = impl.release();
        return;
    }

    FsFileSystem* fs = sdCard();
    Result rc;
    if (mode == Mode::Create) {
        askCard(path, "delete file", [&] { return fsFsDeleteFile(fs, inner(path)); });
        rc = askCard(path, "create file", [&] { return fsFsCreateFile(fs, inner(path), 0, sizeHint >= LargestPlainFile ? FsCreateOption_BigFile : 0); });
        if (R_FAILED(rc))
            fail(rc, format("Couldn't create '%s' (%s)", path.c_str(), codeOf(rc).c_str()));
    } else if (mode == Mode::Sized) {
        u32 option = sizeHint >= LargestPlainFile ? FsCreateOption_BigFile : 0;
        rc = askCard(path, "create file", [&] { return fsFsCreateFile(fs, inner(path), sizeHint, option); });
        if (R_FAILED(rc)) {
            askCard(path, "delete file", [&] { return fsFsDeleteFile(fs, inner(path)); });
            rc = askCard(path, "create file", [&] { return fsFsCreateFile(fs, inner(path), sizeHint, option); });
        }
        if (R_FAILED(rc))
            fail(rc, format("Couldn't create '%s' (%s)", path.c_str(), codeOf(rc).c_str()));
    }
    std::unique_ptr<Impl> impl(new Impl);
    impl->path = path;
    u32 openMode = mode == Mode::Read ? FsOpenMode_Read : mode == Mode::Sized ? FsOpenMode_Write : FsOpenMode_Read | FsOpenMode_Write | FsOpenMode_Append;
    rc = askCard(path, "open file", [&] { return fsFsOpenFile(fs, inner(path), openMode, &impl->file); });
    if (R_FAILED(rc))
        fail(rc, format("Couldn't open '%s' (%s)", path.c_str(), codeOf(rc).c_str()));
    _impl = impl.release();
}

File::~File() { close(); }

i64 File::size() {
    if (_impl->remote)
        return _impl->remote->size();
    if (_impl->usb) {
        struct stat info;
        if (fstat(_impl->descriptor, &info) != 0)
            throw ToolError(format("Couldn't read the size of '%s' (%s)", _impl->path.c_str(), strerror(errno)));
        return info.st_size;
    }
    s64 size = 0;
    Result rc = fsFileGetSize(&_impl->file, &size);
    if (R_FAILED(rc))
        fail(rc, format("Couldn't read the size of '%s' (%s)", _impl->path.c_str(), codeOf(rc).c_str()));
    return size;
}

void File::setSize(i64 size) {
    if (_impl->remote)
        throw ToolError("PC 上的文件只能读取");
    if (_impl->usb) {
        if (ftruncate(_impl->descriptor, size) != 0)
            throw ToolError(format("Couldn't resize '%s' (%s)", _impl->path.c_str(), strerror(errno)));
        return;
    }
    Result rc = fsFileSetSize(&_impl->file, size);
    if (R_FAILED(rc))
        fail(rc, format("Couldn't resize '%s' (%s)", _impl->path.c_str(), codeOf(rc).c_str()));
}

bool File::reserve(i64 size) {
    if (_impl->remote)
        return false;
    // In one go, the card can keep it in one piece, and doesn't have to find more room with every write.
    return !_impl->usb && R_SUCCEEDED(fsFileSetSize(&_impl->file, size));
}

void File::read(i64 offset, void* buffer, size_t count) {
    if (_impl->remote) {
        _impl->remote->read(offset, buffer, count);
        return;
    }
    giveWay(count);
    u8* out = (u8*)buffer;
    if (_impl->usb) {
        mutexLock(&_impl->position);
        bool ok = lseek(_impl->descriptor, offset, SEEK_SET) >= 0;
        while (ok && count > 0) {
            ssize_t got = ::read(_impl->descriptor, out, count);
            ok = got > 0;
            if (ok) {
                out += got;
                count -= (size_t)got;
            }
        }
        mutexUnlock(&_impl->position);
        if (!ok)
            throw ToolError(format("Couldn't read '%s'. If it's on a USB drive, is it still plugged in? (%s)", _impl->path.c_str(), strerror(errno)));
        return;
    }
    while (count > 0) {
        u64 got = 0;
        Result rc;
        {
            InFlight going;
            rc = fsFileRead(&_impl->file, offset, out, count, FsReadOption_None, &got);
        }
        if (isRefusal(rc))
            noteRefusal("read file", _impl->path, rc, g_inFlight, out, count);
        if (R_FAILED(rc))
            fail(rc, format("Couldn't read '%s' (%s)", _impl->path.c_str(), codeOf(rc).c_str()));
        if (got == 0)
            throw ToolError(format("'%s' ended earlier than expected", _impl->path.c_str()));
        out += got;
        offset += (i64)got;
        count -= got;
    }
}

void File::write(i64 offset, const void* buffer, size_t count) {
    if (_impl->remote)
        throw ToolError("PC 上的文件只能读取");
    if (count == 0)
        return;
    giveWay(count);
    if (_impl->usb) {
        const u8* in = (const u8*)buffer;
        mutexLock(&_impl->position);
        bool ok = lseek(_impl->descriptor, offset, SEEK_SET) >= 0;
        while (ok && count > 0) {
            ssize_t put = ::write(_impl->descriptor, in, count);
            ok = put > 0;
            if (ok) {
                in += put;
                count -= (size_t)put;
            }
        }
        mutexUnlock(&_impl->position);
        if (!ok)
            throw ToolError(format("Couldn't write to '%s': the drive may be full or taken out (%s)", _impl->path.c_str(), strerror(errno)));
        return;
    }
    Result rc;
    {
        InFlight going;
        rc = fsFileWrite(&_impl->file, offset, buffer, count, FsWriteOption_None);
    }
    if (isRefusal(rc))
        noteRefusal("write file", _impl->path, rc, g_inFlight, buffer, count);
    if (R_FAILED(rc))
        fail(rc, format("Couldn't write to '%s'%s (%s)", _impl->path.c_str(), isDamagedFileSystem(R_MODULE(rc), R_DESCRIPTION(rc)) ? "" : ": the card may be full",
                         codeOf(rc).c_str()));
}

void File::flush() {
    if (_impl && _impl->remote)
        return;
    if (!_impl)
        return;
    // Said like a write that fails, as it's what was written not reaching the card: the journal, or the last of an NSP.
    if (_impl->usb) {
        if (fsync(_impl->descriptor) != 0)
            throw ToolError(format("Couldn't finish writing '%s': the drive may be full or taken out (%s)", _impl->path.c_str(), strerror(errno)));
        return;
    }
    Result rc;
    {
        InFlight going;
        rc = fsFileFlush(&_impl->file);
    }
    if (isRefusal(rc))
        noteRefusal("flush file", _impl->path, rc, g_inFlight, nullptr, 0);
    if (R_FAILED(rc))
        fail(rc, format("Couldn't finish writing '%s'%s (%s)", _impl->path.c_str(), isDamagedFileSystem(R_MODULE(rc), R_DESCRIPTION(rc)) ? "" : ": the card may be full",
                         codeOf(rc).c_str()));
}

void File::close() {
    if (_impl && _impl->remote) {
        delete _impl;
        _impl = nullptr;
        return;
    }
    if (!_impl)
        return;
    if (_impl->usb)
        ::close(_impl->descriptor);
    else
        fsFileClose(&_impl->file);
    delete _impl;
    _impl = nullptr;
}

Presence presence(const std::string& path) {
    if (remote::isRemote(path))
        return remote::isDirectory(path) || remote::fileSize(path) >= 0 ? Presence::There : Presence::Missing;
    if (!onCard(path)) {
        struct stat info;
        if (stat(path.c_str(), &info) == 0)
            return Presence::There;
        return isMissingErrno(errno) ? Presence::Missing : Presence::CantTell;
    }
    FsDirEntryType type;
    Result rc = askCard(path, "check", [&] { return fsFsGetEntryType(sdCard(), inner(path), &type); });
    return R_SUCCEEDED(rc) ? Presence::There : isNotFound(rc) ? Presence::Missing : Presence::CantTell;
}

bool exists(const std::string& path) { return presence(path) != Presence::Missing; }

bool isDirectory(const std::string& path) {
    if (remote::isRemote(path))
        return remote::isDirectory(path);
    if (!onCard(path)) {
        struct stat info;
        return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
    }
    FsDirEntryType type;
    return R_SUCCEEDED(askCard(path, "check", [&] { return fsFsGetEntryType(sdCard(), inner(path), &type); })) && type == FsDirEntryType_Dir;
}

Presence folderPresence(const std::string& path) {
    if (remote::isRemote(path))
        return remote::folderPresence(path);
    if (!onCard(path)) {
        struct stat info;
        if (stat(path.c_str(), &info) == 0)
            return S_ISDIR(info.st_mode) ? Presence::There : Presence::Missing;
        return isMissingErrno(errno) ? Presence::Missing : Presence::CantTell;
    }
    FsDirEntryType type;
    Result rc = askCard(path, "check", [&] { return fsFsGetEntryType(sdCard(), inner(path), &type); });
    if (R_SUCCEEDED(rc))
        return type == FsDirEntryType_Dir ? Presence::There : Presence::Missing;
    return isNotFound(rc) ? Presence::Missing : Presence::CantTell;
}

std::vector<DirEntry> list(const std::string& path, bool sizes, bool* refused) {
    if (remote::isRemote(path))
        return remote::list(path, refused);
    giveWay();
    std::vector<DirEntry> entries;
    if (!onCard(path)) {
        DIR* folder = opendir(path.c_str());
        if (!folder) {
            if (refused && !isMissingErrno(errno))
                *refused = true;
            return entries;
        }
        std::string base = path;
        if (base.back() != '/')
            base += '/';
        while (true) {
            errno = 0;
            struct dirent* item = readdir(folder);
            if (!item) {
                // The end, or reading it went wrong, which it says by errno.
                if (refused && errno != 0)
                    *refused = true;
                break;
            }
            if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, ".."))
                continue;
            DirEntry entry;
            entry.name = item->d_name;
            // Asking a USB drive about each file means a trip to the disk for each one, which adds up over a whole drive.
            if (sizes || item->d_type == DT_UNKNOWN) {
                struct stat info;
                if (stat((base + entry.name).c_str(), &info) != 0) {
                    // Left off a list that then says it isn't whole, rather than as if it weren't there.
                    if (refused)
                        *refused = true;
                    continue;
                }
                entry.isDirectory = S_ISDIR(info.st_mode);
                entry.size = entry.isDirectory ? 0 : info.st_size;
            } else {
                entry.isDirectory = item->d_type == DT_DIR;
                entry.size = entry.isDirectory ? 0 : -1;
            }
            entries.push_back(entry);
        }
        closedir(folder);
        return entries;
    }
    FsDir dir;
    // Having to say how large each file is takes the card extra work.
    Result rc = askCard(path, "open folder", [&] { return fsFsOpenDirectory(sdCard(), inner(path), FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles | (sizes ? 0 : FsDirOpenMode_NoFileSize), &dir); });
    if (R_FAILED(rc)) {
        // A folder that isn't there is empty; one that's there but couldn't be opened isn't known to be.
        if (refused && !isNotFound(rc))
            *refused = true;
        return entries;
    }
    constexpr int Batch = 32;
    std::unique_ptr<FsDirectoryEntry[]> batch(new FsDirectoryEntry[Batch]);
    while (true) {
        s64 count = 0;
        Result read;
        {
            InFlight going;
            read = fsDirRead(&dir, &count, Batch, batch.get());
        }
        if (isRefusal(read))
            noteRefusal("read folder", path, read, g_inFlight, batch.get(), sizeof(FsDirectoryEntry) * Batch);
        // Turned down, or failing, part of the way through: what came before isn't all there is.
        if (R_FAILED(read)) {
            if (refused)
                *refused = true;
            break;
        }
        if (count <= 0)
            break;
        for (s64 i = 0; i < count; i++) {
            DirEntry entry;
            entry.name = batch[(size_t)i].name;
            entry.isDirectory = batch[(size_t)i].type == FsDirEntryType_Dir;
            entry.size = entry.isDirectory || sizes ? batch[(size_t)i].file_size : -1;
            entries.push_back(entry);
        }
    }
    fsDirClose(&dir);
    return entries;
}

i64 fileSize(const std::string& path) {
    if (remote::isRemote(path))
        return remote::fileSize(path);
    if (!onCard(path)) {
        struct stat info;
        return stat(path.c_str(), &info) == 0 ? (i64)info.st_size : -1;
    }
    FsFile file;
    if (R_FAILED(askCard(path, "open file", [&] { return fsFsOpenFile(sdCard(), inner(path), FsOpenMode_Read, &file); })))
        return -1;
    s64 size = -1;
    fsFileGetSize(&file, &size);
    fsFileClose(&file);
    return size;
}

void makeDirectories(const std::string& path) {
    if (remote::isRemote(path))
        return;
    if (!onCard(path)) {
        // Past the drive's name, each folder on the way.
        size_t at = path.find(":/");
        at = at == std::string::npos ? 0 : at + 2;
        while (at <= path.size()) {
            size_t slash = path.find('/', at);
            if (slash == std::string::npos)
                slash = path.size();
            if (slash > at)
                mkdir(path.substr(0, slash).c_str(), 0777);
            at = slash + 1;
        }
        return;
    }
    FsFileSystem* fs = sdCard();
    // Past the card's name.
    std::string part;
    for (const char* c = path.c_str() + 5;; c++) {
        if ((*c == '/' || !*c) && !part.empty())
            askCard(part, "make folder", [&] { return fsFsCreateDirectory(fs, FsPath(part.c_str())); });
        if (!*c)
            break;
        part += *c;
    }
}

void makeDirectory(const std::string& path) {
    if (onCard(path))
        askCard(path, "make folder", [&] { return fsFsCreateDirectory(sdCard(), inner(path)); });
    else
        mkdir(path.c_str(), 0777);
}

// What's refused for a moment is asked again, as a delete that doesn't happen leaves the file in the way of whatever comes next.
bool removeFile(const std::string& path) {
    return onCard(path) ? R_SUCCEEDED(askCard(path, "delete file", [&] { return fsFsDeleteFile(sdCard(), inner(path)); })) : unlink(path.c_str()) == 0;
}

bool removeDirectory(const std::string& path) {
    return onCard(path) ? R_SUCCEEDED(askCard(path, "delete folder", [&] { return fsFsDeleteDirectory(sdCard(), inner(path)); })) : rmdir(path.c_str()) == 0;
}

bool removeTree(const std::string& path) {
    if (onCard(path))
        return R_SUCCEEDED(askCard(path, "delete folder and all in it", [&] { return fsFsDeleteDirectoryRecursively(sdCard(), inner(path)); }));
    for (auto& entry : list(path)) {
        std::string inside = path + "/" + entry.name;
        if (entry.isDirectory)
            removeTree(inside);
        else
            unlink(inside.c_str());
    }
    return rmdir(path.c_str()) == 0;
}

void renameFile(const std::string& from, const std::string& to) {
    if (!onCard(from)) {
        if (rename(from.c_str(), to.c_str()) != 0)
            throw ToolError(format("Couldn't rename '%s' (%s)", from.c_str(), strerror(errno)));
        return;
    }
    Result rc = askCard(from, "rename", [&] { return fsFsRenameFile(sdCard(), inner(from), inner(to)); });
    if (R_FAILED(rc))
        fail(rc, format("Couldn't rename '%s' (%s)", from.c_str(), codeOf(rc).c_str()));
}

i64 freeSpace(const std::string& path) {
    if (remote::isRemote(path))
        return 0;
    if (!onCard(path)) {
        struct statvfs info;
        return statvfs(path.c_str(), &info) == 0 ? (i64)info.f_bavail * (i64)info.f_frsize : 0;
    }
    s64 free = 0;
    return R_SUCCEEDED(askCard(path, "free space of", [&] { return fsFsGetFreeSpace(sdCard(), FsPath("/"), &free); })) ? free : 0;
}

i64 totalSpace(const std::string& path) {
    if (!onCard(path)) {
        struct statvfs info;
        return statvfs(path.c_str(), &info) == 0 ? (i64)info.f_blocks * (i64)info.f_frsize : 0;
    }
    s64 total = 0;
    return R_SUCCEEDED(askCard(path, "total space of", [&] { return fsFsGetTotalSpace(sdCard(), FsPath("/"), &total); })) ? total : 0;
}

i64 largestFile(const std::string& path) {
    constexpr i64 Fat32Limit = 0xFFFFFFFFLL;
    if (onCard(path))
        return INT64_MAX;
    UsbHsFsDevice device;
    if (!usbHsFsGetDeviceByPath(path.c_str(), &device))
        return INT64_MAX;
    bool fat = device.fs_type == UsbHsFsDeviceFileSystemType_FAT12 || device.fs_type == UsbHsFsDeviceFileSystemType_FAT16 || device.fs_type == UsbHsFsDeviceFileSystemType_FAT32;
    return fat ? Fat32Limit : INT64_MAX;
}

i64 spareMemory() {
    // What it takes to work is well under this, and what's left over is worth using. Opened from the album, there's nothing to spare.
    constexpr i64 KeptForWork = 768LL << 20;
    i64 heap = fake_heap_end - fake_heap_start;
    return std::max<i64>(0, heap - KeptForWork);
}

i64 workMemory() { return fake_heap_end - fake_heap_start; }

HeapUse heapUse(bool findLargest) {
    HeapUse use;
    // What malloc hasn't taken from the heap yet is as free as what it has and isn't using.
    struct mallinfo info = mallinfo();
    use.inUse = (i64)info.uordblks;
    use.free = workMemory() - use.inUse;
    if (findLargest) {
        i64 low = 0, high = use.free;
        while (high - low > (1 << 20)) {
            i64 size = low + (high - low) / 2;
            if (void* piece = malloc((size_t)size)) {
                ::free(piece);
                low = size;
            } else {
                high = size;
            }
        }
        use.largestPiece = low;
    }
    return use;
}

namespace {

/// The last two times malloc said no, how much was asked for and from where, as places in this program (from its start), which
/// addr2line on the .elf turns into lines of code. Taken as it happens, so it can't allocate anything.
struct Refusal {
    size_t size = 0;
    int frames = 0;
    uintptr_t at[12] = {};
};
Refusal g_mallocRefusals[2];
std::atomic<int> g_mallocRefusalCount{0};

struct Walk {
    Refusal* refusal;
};

_Unwind_Reason_Code walkStep(_Unwind_Context* context, void* argument) {
    Refusal& refusal = *((Walk*)argument)->refusal;
    if (refusal.frames >= (int)(sizeof(refusal.at) / sizeof(refusal.at[0])))
        return _URC_END_OF_STACK;
    refusal.at[refusal.frames++] = (uintptr_t)_Unwind_GetIP(context) - (uintptr_t)&__start__;
    return _URC_NO_REASON;
}

void noteRefusal(size_t size) {
    Refusal& refusal = g_mallocRefusals[g_mallocRefusalCount.fetch_add(1) % 2];
    refusal.size = size;
    refusal.frames = 0;
    Walk walk{&refusal};
    _Unwind_Backtrace(walkStep, &walk);
}

}  // namespace

void noteRefusalForLog(size_t size) { noteRefusal(size); }

std::string refusedAllocations() {
    int count = g_mallocRefusalCount.load();
    std::string text;
    for (int back = 0; back < std::min(count, 2); back++) {
        const Refusal& refusal = g_mallocRefusals[(count - 1 - back) % 2];
        char line[64];
        snprintf(line, sizeof line, "%s%zu bytes asked for, from", back == 0 ? "" : "; before that ", refusal.size);
        text += line;
        for (int i = 0; i < refusal.frames; i++) {
            snprintf(line, sizeof line, " %lx", (unsigned long)refusal.at[i]);
            text += line;
        }
    }
    g_mallocRefusalCount = 0;
    return text;
}

void commit(const std::string& path) {
    if (onCard(path))
        askCard(path, "commit", [&] { return fsFsCommit(sdCard()); });
}

struct Mutex::Impl {
    ::Mutex value;
};

Mutex::Mutex() : _impl(new Impl) { mutexInit(&_impl->value); }
Mutex::~Mutex() { delete _impl; }
void Mutex::lock() { mutexLock(&_impl->value); }
void Mutex::unlock() { mutexUnlock(&_impl->value); }
bool Mutex::tryLock() { return mutexTryLock(&_impl->value); }

struct CondVar::Impl {
    ::CondVar value;
};

CondVar::CondVar() : _impl(new Impl) { condvarInit(&_impl->value); }
CondVar::~CondVar() { delete _impl; }
void CondVar::wait(Mutex& mutex) { condvarWait(&_impl->value, &mutex._impl->value); }
void CondVar::wakeOne() { condvarWakeOne(&_impl->value); }
void CondVar::wakeAll() { condvarWakeAll(&_impl->value); }

struct Thread::Impl {
    ::Thread value;
    std::function<void()> work;
};

Thread::~Thread() { join(); }

void Thread::start(std::function<void()> work, int core, size_t stackSize, bool mostlyWaits) {
    std::unique_ptr<Impl> impl(new Impl);
    impl->work = std::move(work);
    size_t aligned = (stackSize + 0xFFF) & ~(size_t)0xFFF;
    Result rc = threadCreate(&impl->value, [](void* argument) { ((Impl*)argument)->work(); }, impl.get(), nullptr, aligned, mostlyWaits ? WaitingPriority : WorkPriority, core >= 0 ? core : -2);
    if (R_FAILED(rc))
        fail(rc, format("Couldn't start a thread (%s)", codeOf(rc).c_str()));
    rc = threadStart(&impl->value);
    if (R_FAILED(rc)) {
        threadClose(&impl->value);
        fail(rc, format("Couldn't start a thread (%s)", codeOf(rc).c_str()));
    }
    _impl = impl.release();
}

void Thread::join() {
    if (!_impl)
        return;
    threadWaitForExit(&_impl->value);
    threadClose(&_impl->value);
    delete _impl;
    _impl = nullptr;
}

int workerCount() { return __builtin_popcountll(allowedCores()); }

int workerCore(int index) {
    u64 mask = allowedCores();
    int wanted = index % workerCount();
    for (int core = 0; core < 4; core++) {
        if (mask >> core & 1) {
            if (wanted-- == 0)
                return core;
        }
    }
    return -1;
}

void setInForeground(bool foreground) { g_inForeground = foreground; }

bool workerShouldWait(int worker) { return !g_inForeground && worker != 0 && workerCore(worker) == SystemCore; }

}  // namespace hbp::sys

// Every allocation goes through here, as it would anyway, so that when there's no room the log can say how much was asked for and where.
void* operator new(size_t size) {
    for (;;) {
        if (void* piece = malloc(size ? size : 1))
            return piece;
        hbp::sys::noteRefusalForLog(size);
        std::new_handler handler = std::get_new_handler();
        if (!handler)
            throw std::bad_alloc();
        handler();
    }
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* piece) noexcept { free(piece); }
void operator delete[](void* piece) noexcept { free(piece); }
void operator delete(void* piece, size_t) noexcept { free(piece); }
void operator delete[](void* piece, size_t) noexcept { free(piece); }
