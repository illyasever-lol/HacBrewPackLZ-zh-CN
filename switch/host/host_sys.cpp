// The engine's view of a PC, so that all of it can be run and tested here.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <random>
#include <thread>

#include "../core/sys.h"
#include "../core/remote.h"

namespace fs = std::filesystem;

namespace hbp::sys {

double now() {
    using Clock = std::chrono::steady_clock;
    static const auto start = Clock::now();
    return std::chrono::duration<double>(Clock::now() - start).count();
}

void sleepMs(int milliseconds) { std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds)); }

void randomBytes(void* out, size_t size) {
    static std::mutex lock;
    static std::random_device device;
    std::lock_guard<std::mutex> guard(lock);
    u8* b = (u8*)out;
    for (size_t i = 0; i < size; i++)
        b[i] = (u8)device();
}

static fs::path toPath(const std::string& path) { return fs::u8path(path); }

struct File::Impl {
    std::shared_ptr<remote::RemoteFile> remote;
    std::mutex lock;
    FILE* file = nullptr;
    std::string path;
};

File::File(const std::string& path, Mode mode, i64) : _impl(new Impl) {
    if (remote::isRemote(path)) {
        if (mode != Mode::Read)
            throw ToolError("PC 上的文件只能读取");
        _impl->path = path;
        _impl->remote = remote::open(path);
        return;
    }
    _impl->path = path;
#ifdef _WIN32
    const wchar_t* flags = mode == Mode::Read ? L"rb" : mode == Mode::Create || mode == Mode::CreateOnce || mode == Mode::Sized ? L"w+b" : L"r+b";
    _impl->file = _wfopen(toPath(path).c_str(), flags);
#else
    const char* flags = mode == Mode::Read ? "rb" : mode == Mode::Create || mode == Mode::CreateOnce || mode == Mode::Sized ? "w+b" : "r+b";
    _impl->file = fopen(path.c_str(), flags);
#endif
    if (!_impl->file) {
        delete _impl;
        _impl = nullptr;
        throw ToolError(format("Couldn't open '%s'", path.c_str()));
    }
}

File::~File() { close(); }

i64 File::size() {
    if (_impl->remote)
        return _impl->remote->size();
    std::lock_guard<std::mutex> guard(_impl->lock);
#ifdef _WIN32
    _fseeki64(_impl->file, 0, SEEK_END);
    return _ftelli64(_impl->file);
#else
    fseeko(_impl->file, 0, SEEK_END);
    return ftello(_impl->file);
#endif
}

void File::setSize(i64 size) {
    flush();
    std::error_code error;
    fs::resize_file(toPath(_impl->path), (uintmax_t)size, error);
    if (error)
        throw ToolError(format("Couldn't resize '%s'", _impl->path.c_str()));
}

bool File::reserve(i64) { return false; }

static void seekTo(FILE* file, i64 offset) {
#ifdef _WIN32
    _fseeki64(file, offset, SEEK_SET);
#else
    fseeko(file, offset, SEEK_SET);
#endif
}

void File::read(i64 offset, void* buffer, size_t count) {
    if (_impl->remote) {
        _impl->remote->read(offset, buffer, count);
        return;
    }
    if (count == 0)
        return;
    std::lock_guard<std::mutex> guard(_impl->lock);
    seekTo(_impl->file, offset);
    if (fread(buffer, 1, count, _impl->file) != count)
        throw ToolError(format("'%s' ended earlier than expected", _impl->path.c_str()));
}

void File::write(i64 offset, const void* buffer, size_t count) {
    if (_impl->remote)
        throw ToolError("PC 上的文件只能读取");
    if (count == 0)
        return;
    std::lock_guard<std::mutex> guard(_impl->lock);
    seekTo(_impl->file, offset);
    if (fwrite(buffer, 1, count, _impl->file) != count)
        throw ToolError(format("Couldn't write to '%s': the disk may be full", _impl->path.c_str()));
}

void File::flush() {
    if (_impl)
        fflush(_impl->file);
}

void File::close() {
    if (_impl && _impl->remote) {
        delete _impl;
        _impl = nullptr;
        return;
    }
    if (!_impl)
        return;
    fclose(_impl->file);
    delete _impl;
    _impl = nullptr;
}

bool exists(const std::string& path) {
    if (remote::isRemote(path))
        return remote::isDirectory(path) || remote::fileSize(path) >= 0;
    std::error_code error;
    return fs::exists(toPath(path), error);
}

bool isDirectory(const std::string& path) {
    if (remote::isRemote(path))
        return remote::isDirectory(path);
    std::error_code error;
    return fs::is_directory(toPath(path), error);
}

std::vector<DirEntry> list(const std::string& path, bool sizes) {
    if (remote::isRemote(path))
        return remote::list(path);
    // For the tests: a folder with "slow" in its path takes this long to list, as one on a slow card or drive can.
    const char* slow = getenv("HBP_TEST_SLOW_LIST_MS");
    if (slow && *slow && path.find("slow") != std::string::npos)
        std::this_thread::sleep_for(std::chrono::milliseconds(atoi(slow)));
    std::vector<DirEntry> entries;
    std::error_code error;
    for (auto& item : fs::directory_iterator(toPath(path), error)) {
        DirEntry entry;
        entry.name = item.path().filename().u8string();
        entry.isDirectory = item.is_directory();
        entry.size = entry.isDirectory ? 0 : sizes ? (i64)item.file_size(error) : -1;
        entries.push_back(entry);
    }
    return entries;
}

i64 fileSize(const std::string& path) {
    if (remote::isRemote(path))
        return remote::fileSize(path);
    std::error_code error;
    auto size = fs::file_size(toPath(path), error);
    return error ? -1 : (i64)size;
}

void makeDirectories(const std::string& path) {
    if (remote::isRemote(path))
        return;
    std::error_code error;
    fs::create_directories(toPath(path), error);
}

i64 outOfHandlesCount() { return 0; }

void makeDirectory(const std::string& path) {
    std::error_code error;
    fs::create_directory(toPath(path), error);
}

bool removeFile(const std::string& path) {
    std::error_code error;
    return fs::remove(toPath(path), error);
}

bool removeDirectory(const std::string& path) {
    std::error_code error;
    return fs::remove(toPath(path), error);
}

bool removeTree(const std::string& path) {
    std::error_code error;
    return fs::remove_all(toPath(path), error) > 0;
}

void renameFile(const std::string& from, const std::string& to) {
    std::error_code error;
    fs::rename(toPath(from), toPath(to), error);
    if (error)
        throw ToolError(format("Couldn't rename '%s'", from.c_str()));
}

i64 freeSpace(const std::string& path) {
    if (remote::isRemote(path))
        return 0;
    std::error_code error;
    auto info = fs::space(toPath(path), error);
    return error ? 0 : (i64)info.available;
}

i64 totalSpace(const std::string& path) {
    std::error_code error;
    auto info = fs::space(toPath(path), error);
    return error ? 0 : (i64)info.capacity;
}

i64 largestFile(const std::string&) { return INT64_MAX; }

i64 spareMemory() {
    // There's only as much as a test asks for.
    const char* wanted = getenv("HBP_TEST_SPARE_MEMORY");
    return wanted ? atoll(wanted) : 0;
}

i64 workMemory() {
    const char* wanted = getenv("HBP_TEST_WORK_MEMORY");
    return wanted && *wanted ? atoll(wanted) : INT64_MAX;
}

void commit(const std::string&) {}

struct Mutex::Impl {
    std::mutex value;
};
Mutex::Mutex() : _impl(new Impl) {}
Mutex::~Mutex() { delete _impl; }
void Mutex::lock() { _impl->value.lock(); }
void Mutex::unlock() { _impl->value.unlock(); }

struct CondVar::Impl {
    std::condition_variable_any value;
};
CondVar::CondVar() : _impl(new Impl) {}
CondVar::~CondVar() { delete _impl; }
void CondVar::wait(Mutex& mutex) { _impl->value.wait(mutex); }
void CondVar::wakeOne() { _impl->value.notify_one(); }
void CondVar::wakeAll() { _impl->value.notify_all(); }

struct Thread::Impl {
    std::thread value;
};

Thread::~Thread() { join(); }

void Thread::start(std::function<void()> work, int, size_t, bool) {
    _impl = new Impl;
    _impl->value = std::thread(std::move(work));
}

void Thread::join() {
    if (!_impl)
        return;
    _impl->value.join();
    delete _impl;
    _impl = nullptr;
}

int workerCount() { return std::max(1, (int)std::thread::hardware_concurrency()); }
int workerCore(int) { return -1; }
void setInForeground(bool) {}
void giveWay(size_t) {}
bool workerShouldWait(int) { return false; }

}  // namespace hbp::sys
