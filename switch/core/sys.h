// The little the engine needs from the system it runs on: files, threads and time. nx_sys.cpp does it for the console, and
// host_sys.cpp for a PC, where the whole engine can be tested against the C# tool.
#pragma once

#include "common.h"

namespace hbp::sys {

double now();
void sleepMs(int milliseconds);
void randomBytes(void* out, size_t size);

class File {
public:
    /// Sized is Create for a file that's most likely not there yet and whose size is known, given as the hint, which takes the card a good deal less work.
    /// CreateOnce doesn't ask again if the system says it's out of handles, for when there's somewhere else to go instead.
    enum class Mode { Read, Create, CreateOnce, Sized, Update };

    File() = default;
    File(const std::string& path, Mode mode, i64 sizeHint = 0);
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    i64 size();
    void setSize(i64 size);
    /// It's going to come to about this size. Where that helps, the room gets set aside now, in one piece, and setSize puts it right at the
    /// end. False if nothing was done.
    bool reserve(i64 size);
    /// Reads all of it or throws. Any number of threads can read at once.
    void read(i64 offset, void* buffer, size_t count);
    void write(i64 offset, const void* buffer, size_t count);
    void flush();
    void close();

private:
    struct Impl;
    Impl* _impl = nullptr;
};

struct DirEntry {
    std::string name;
    bool isDirectory = false;
    /// Negative if it wasn't asked for and finding out would have taken a while.
    i64 size = 0;
};

bool exists(const std::string& path);
bool isDirectory(const std::string& path);
/// What's in a folder. Sizes only come with it if they cost nothing, or if they're asked for.
std::vector<DirEntry> list(const std::string& path, bool sizes = true);
/// Negative if it isn't there.
i64 fileSize(const std::string& path);
/// How many times the system has said it was out of handles, 2001-0106, and been asked again. For the log.
i64 outOfHandlesCount();

/// How many threads wait on files at once. With custom firmware, everything goes through its fs.mitm module, shared with the whole system, which has
/// been seen to abort, and take the console with it, under more than this. It isn't worth finding out how much more it would take.
constexpr int FileThreads = 3;

void makeDirectories(const std::string& path);
/// Just the one, for when what it goes in is known to be there. With a great many of them, that adds up.
void makeDirectory(const std::string& path);
bool removeFile(const std::string& path);
bool removeTree(const std::string& path);
/// Only if there's nothing left in it.
bool removeDirectory(const std::string& path);
void renameFile(const std::string& from, const std::string& to);
i64 freeSpace(const std::string& path);
i64 totalSpace(const std::string& path);
/// The largest file a folder can hold.
i64 largestFile(const std::string& path);
/// How much is worth keeping in memory for a while, on top of what work takes up.
i64 spareMemory();
/// How much there is to work with, all told.
i64 workMemory();
/// Makes sure what was written is really on the card.
void commit(const std::string& path);

class Mutex {
public:
    Mutex();
    ~Mutex();
    Mutex(const Mutex&) = delete;
    void lock();
    void unlock();

private:
    friend class CondVar;
    struct Impl;
    Impl* _impl;
};

class Lock {
public:
    explicit Lock(Mutex& mutex) : _mutex(mutex) { _mutex.lock(); }
    ~Lock() { _mutex.unlock(); }
    Lock(const Lock&) = delete;

private:
    Mutex& _mutex;
};

class CondVar {
public:
    CondVar();
    ~CondVar();
    CondVar(const CondVar&) = delete;
    void wait(Mutex& mutex);
    void wakeOne();
    void wakeAll();

private:
    struct Impl;
    Impl* _impl;
};

class Thread {
public:
    Thread() = default;
    ~Thread();
    Thread(const Thread&) = delete;
    /// Core -1 means wherever the system likes. The work should catch what it throws. A thread that mostly waits for the card gets
    /// ahead of those doing the sums whenever it has something to do.
    void start(std::function<void()> work, int core = -1, size_t stackSize = 1 << 20, bool mostlyWaits = false);
    void join();

private:
    struct Impl;
    Impl* _impl = nullptr;
};

/// How many threads are worth compressing with.
int workerCount();
/// Whether the console's on this program or something else. Reading and writing is mostly done by the system's file service, on the system's
/// own core, which is also what the home menu runs on. So while something else is on the screen, all of it goes more slowly.
void setInForeground(bool foreground);
/// Called with whatever's about to go through the system's file service, or some other service of its.
void giveWay(size_t bytes = 0);
/// A worker that should leave the processor alone for now, as it's on a core that something else needs more.
bool workerShouldWait(int worker);
/// Which core a worker belongs on, or -1 if that doesn't matter here.
int workerCore(int index);

}  // namespace hbp::sys
