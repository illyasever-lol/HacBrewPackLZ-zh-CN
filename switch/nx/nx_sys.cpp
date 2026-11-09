// The engine's view of the console: the SD card, USB drives, threads and time.
#include <dirent.h>
#include <fcntl.h>
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

#include "../core/sys.h"
#include "../core/remote.h"

extern "C" char* fake_heap_start;
extern "C" char* fake_heap_end;

namespace hbp::sys {

namespace {

/// The main thread has 0x2C, which is what keeps the screen responsive. The kernel only takes turns among threads of the same priority
/// at the lowest ones, so that's where the sums get done, and whatever waits for the card gets ahead of them when it has something to do.
constexpr int WorkPriority = 0x3B;
constexpr int WaitingPriority = 0x2D;

std::string codeOf(Result rc) { return format("%04u-%04u", 2000 + R_MODULE(rc), R_DESCRIPTION(rc)); }

FsFileSystem* sdCard() {
    FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc");
    if (!fs)
        throw ToolError("无法访问 SD 卡");
    return fs;
}

/// The SD card gets used directly, as the file system it comes from is quick to work with and gives real error codes. Anything else is a USB drive.
bool onCard(const std::string& path) { return path.compare(0, 5, "sdmc:") == 0; }

/// The file system doesn't want to hear the SD card's name.
const char* inner(const std::string& path) { return onCard(path) ? path.c_str() + 5 : path.c_str(); }

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

std::atomic<i64> g_outOfHandles{0};

/// Creating and opening files has been seen to come back as being out of handles, 2001-0106, at no particular time, even when hardly anything else
/// was going on. It hasn't been worked out why, and asking again is worth a try before giving up on the file.
template <typename Attempt>
Result askAgainIfOutOfHandles(Attempt attempt) {
    constexpr Result OutOfHandles = MAKERESULT(1, 106);
    constexpr int MostTries = 10;
    Result rc = attempt();
    for (int i = 0; i < MostTries && rc == OutOfHandles; i++) {
        g_outOfHandles++;
        sleepMs(50);
        rc = attempt();
    }
    return rc;
}

i64 outOfHandlesCount() { return g_outOfHandles; }

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
        int flags = mode == Mode::Read ? O_RDONLY : mode == Mode::Create || mode == Mode::CreateOnce || mode == Mode::Sized ? O_RDWR | O_CREAT | O_TRUNC : O_RDWR;
        impl->descriptor = open(path.c_str(), flags, 0666);
        if (impl->descriptor < 0)
            throw ToolError(format("Couldn't open '%s' (%s)", path.c_str(), strerror(errno)));
        _impl = impl.release();
        return;
    }

    FsFileSystem* fs = sdCard();
    Result rc;
    if (mode == Mode::CreateOnce) {
        fsFsDeleteFile(fs, inner(path));
        rc = fsFsCreateFile(fs, inner(path), 0, 0);
        if (R_FAILED(rc))
            throw ToolError(format("Couldn't create '%s' (%s)", path.c_str(), codeOf(rc).c_str()));
    } else if (mode == Mode::Create) {
        fsFsDeleteFile(fs, inner(path));
        rc = askAgainIfOutOfHandles([&] { return fsFsCreateFile(fs, inner(path), 0, sizeHint >= LargestPlainFile ? FsCreateOption_BigFile : 0); });
        if (R_FAILED(rc))
            throw ToolError(format("Couldn't create '%s' (%s)", path.c_str(), codeOf(rc).c_str()));
    } else if (mode == Mode::Sized) {
        u32 option = sizeHint >= LargestPlainFile ? FsCreateOption_BigFile : 0;
        rc = askAgainIfOutOfHandles([&] { return fsFsCreateFile(fs, inner(path), sizeHint, option); });
        if (R_FAILED(rc)) {
            fsFsDeleteFile(fs, inner(path));
            rc = askAgainIfOutOfHandles([&] { return fsFsCreateFile(fs, inner(path), sizeHint, option); });
        }
        if (R_FAILED(rc))
            throw ToolError(format("Couldn't create '%s' (%s)", path.c_str(), codeOf(rc).c_str()));
    }
    std::unique_ptr<Impl> impl(new Impl);
    impl->path = path;
    rc = askAgainIfOutOfHandles([&] { return fsFsOpenFile(fs, inner(path), mode == Mode::Read ? FsOpenMode_Read : mode == Mode::Sized ? FsOpenMode_Write : FsOpenMode_Read | FsOpenMode_Write | FsOpenMode_Append, &impl->file); });
    if (R_FAILED(rc))
        throw ToolError(format("Couldn't open '%s' (%s)", path.c_str(), codeOf(rc).c_str()));
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
        throw ToolError(format("Couldn't read the size of '%s' (%s)", _impl->path.c_str(), codeOf(rc).c_str()));
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
        throw ToolError(format("Couldn't resize '%s' (%s)", _impl->path.c_str(), codeOf(rc).c_str()));
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
        Result rc = fsFileRead(&_impl->file, offset, out, count, FsReadOption_None, &got);
        if (R_FAILED(rc))
            throw ToolError(format("Couldn't read '%s' (%s)", _impl->path.c_str(), codeOf(rc).c_str()));
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
    Result rc = fsFileWrite(&_impl->file, offset, buffer, count, FsWriteOption_None);
    if (R_FAILED(rc))
        throw ToolError(format("Couldn't write to '%s': the card may be full (%s)", _impl->path.c_str(), codeOf(rc).c_str()));
}

void File::flush() {
    if (_impl && _impl->remote)
        return;
    if (!_impl)
        return;
    if (_impl->usb)
        fsync(_impl->descriptor);
    else
        fsFileFlush(&_impl->file);
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

bool exists(const std::string& path) {
    if (remote::isRemote(path))
        return remote::isDirectory(path) || remote::fileSize(path) >= 0;
    if (!onCard(path)) {
        struct stat info;
        return stat(path.c_str(), &info) == 0;
    }
    FsDirEntryType type;
    return R_SUCCEEDED(fsFsGetEntryType(sdCard(), inner(path), &type));
}

bool isDirectory(const std::string& path) {
    if (remote::isRemote(path))
        return remote::isDirectory(path);
    if (!onCard(path)) {
        struct stat info;
        return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
    }
    // One GetEntryType call is not enough: the FS session is shared with the file scan
    // on another thread, and a single failed call used to make an existing output
    // folder look unreachable. Retry, then open it, which is the authoritative test.
    for (int attempt = 0; attempt < 3; attempt++) {
        FsDirEntryType type;
        if (R_SUCCEEDED(fsFsGetEntryType(sdCard(), inner(path), &type)))
            return type == FsDirEntryType_Dir;
        sys::sleepMs(10 * (attempt + 1));
    }
    FsDir dir;
    if (R_FAILED(fsFsOpenDirectory(sdCard(), inner(path), FsDirOpenMode_ReadDirs, &dir)))
        return false;
    fsDirClose(&dir);
    return true;
}

std::vector<DirEntry> list(const std::string& path, bool sizes) {
    if (remote::isRemote(path))
        return remote::list(path);
    giveWay();
    std::vector<DirEntry> entries;
    if (!onCard(path)) {
        DIR* folder = opendir(path.c_str());
        if (!folder)
            return entries;
        std::string base = path;
        if (base.back() != '/')
            base += '/';
        while (struct dirent* item = readdir(folder)) {
            if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, ".."))
                continue;
            DirEntry entry;
            entry.name = item->d_name;
            // Asking a USB drive about each file means a trip to the disk for each one, which adds up over a whole drive.
            if (sizes || item->d_type == DT_UNKNOWN) {
                struct stat info;
                if (stat((base + entry.name).c_str(), &info) != 0)
                    continue;
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
    if (R_FAILED(askAgainIfOutOfHandles([&] { return fsFsOpenDirectory(sdCard(), inner(path), FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles | (sizes ? 0 : FsDirOpenMode_NoFileSize), &dir); })))
        return entries;
    constexpr int Batch = 32;
    std::unique_ptr<FsDirectoryEntry[]> batch(new FsDirectoryEntry[Batch]);
    while (true) {
        s64 count = 0;
        if (R_FAILED(fsDirRead(&dir, &count, Batch, batch.get())) || count <= 0)
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
    if (R_FAILED(askAgainIfOutOfHandles([&] { return fsFsOpenFile(sdCard(), inner(path), FsOpenMode_Read, &file); })))
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
    // Already there: leave the shared FS session alone. Re-posting CreateDirectory
    // for a folder that exists is what makes the following entry-type query fail.
    if (isDirectory(path))
        return;
    const char* whole = inner(path);
    std::string part;
    for (const char* c = whole;; c++) {
        if ((*c == '/' || !*c) && !part.empty())
            fsFsCreateDirectory(fs, part.c_str());
        if (!*c)
            break;
        part += *c;
    }
}

void makeDirectory(const std::string& path) {
    if (onCard(path))
        fsFsCreateDirectory(sdCard(), inner(path));
    else
        mkdir(path.c_str(), 0777);
}

bool removeFile(const std::string& path) { return onCard(path) ? R_SUCCEEDED(fsFsDeleteFile(sdCard(), inner(path))) : unlink(path.c_str()) == 0; }

bool removeDirectory(const std::string& path) { return onCard(path) ? R_SUCCEEDED(fsFsDeleteDirectory(sdCard(), inner(path))) : rmdir(path.c_str()) == 0; }

bool removeTree(const std::string& path) {
    if (onCard(path))
        return R_SUCCEEDED(fsFsDeleteDirectoryRecursively(sdCard(), inner(path)));
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
    Result rc = fsFsRenameFile(sdCard(), inner(from), inner(to));
    if (R_FAILED(rc))
        throw ToolError(format("Couldn't rename '%s' (%s)", from.c_str(), codeOf(rc).c_str()));
}

i64 freeSpace(const std::string& path) {
    if (remote::isRemote(path))
        return 0;
    if (!onCard(path)) {
        struct statvfs info;
        return statvfs(path.c_str(), &info) == 0 ? (i64)info.f_bavail * (i64)info.f_frsize : 0;
    }
    s64 free = 0;
    return R_SUCCEEDED(fsFsGetFreeSpace(sdCard(), "/", &free)) ? free : 0;
}

i64 totalSpace(const std::string& path) {
    if (!onCard(path)) {
        struct statvfs info;
        return statvfs(path.c_str(), &info) == 0 ? (i64)info.f_blocks * (i64)info.f_frsize : 0;
    }
    s64 total = 0;
    return R_SUCCEEDED(fsFsGetTotalSpace(sdCard(), "/", &total)) ? total : 0;
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

void commit(const std::string& path) {
    if (onCard(path))
        fsFsCommit(sdCard());
}

struct Mutex::Impl {
    ::Mutex value;
};

Mutex::Mutex() : _impl(new Impl) { mutexInit(&_impl->value); }
Mutex::~Mutex() { delete _impl; }
void Mutex::lock() { mutexLock(&_impl->value); }
void Mutex::unlock() { mutexUnlock(&_impl->value); }

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
        throw ToolError(format("Couldn't start a thread (%s)", codeOf(rc).c_str()));
    rc = threadStart(&impl->value);
    if (R_FAILED(rc)) {
        threadClose(&impl->value);
        throw ToolError(format("Couldn't start a thread (%s)", codeOf(rc).c_str()));
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
