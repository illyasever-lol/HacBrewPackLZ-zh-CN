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
    enum class Mode { Read, Create, Sized, Update };

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

/// Whether something's there. Only "未找到" means it isn't: the system can turn the question down for a while (see Refusals), or answer
/// with another error, and then there's no telling.
enum class Presence { There, Missing, CantTell };
Presence presence(const std::string& path);
/// Whether something's there, where not being able to tell counts as there. A refusal is never taken to mean something's missing: that had
/// the welcome come back, remembered games forgotten, and could have had the record of a game left half replaced cleared away.
bool exists(const std::string& path);
bool isDirectory(const std::string& path);
/// Whether there's a folder there: a file of that name counts as none. Where it matters that a folder the system won't answer about isn't
/// taken for one that isn't there, such as a game's romfs folder when it's built.
Presence folderPresence(const std::string& path);
/// What's in a folder. Sizes only come with it if they cost nothing, or if they're asked for. A folder that isn't there lists as empty. One
/// the system turned down, or that couldn't all be read, lists as what it got, and says so through refused, when that's given.
std::vector<DirEntry> list(const std::string& path, bool sizes = true, bool* refused = nullptr);
/// Negative if it isn't there.
i64 fileSize(const std::string& path);
/// How many requests to the card the kernel has turned down, for the log: as being out of handles (2001-0105), and for an invalid memory state
/// (2001-0106), and what the last one was for. Each is asked once (see askCard in nx_sys.cpp).
struct Refusals {
    i64 outOfHandles = 0;
    i64 memoryState = 0;
    std::string lastFor;
    i64 total() const { return outOfHandles + memoryState; }
};
Refusals refusals();
/// Each request the system turned down since this was last asked, the first few each time the app opens, in detail for the log: what was
/// asked, how many other requests were going, and the state of the memory it was asked from. For working out why it happens.
std::vector<std::string> takeRefusalDetails();

/// How many threads wait on files at once: one more than the app's sessions to the card (nx_main.cpp), so there's always a request ready when one
/// comes back. Custom firmware's fs.mitm was seen to abort under more than three on 2 November, from requests turned down for short path buffers,
/// which are full-size since.
constexpr int FileThreads = 4;

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
/// What's taken of it and what's free, and with findLargest the most it can give in one piece, to 1 MB (which takes a moment), for the log
/// when it runs out. Each is -1 where it can't be known. It allocates nothing unless asked for the largest piece.
struct HeapUse {
    i64 inUse = -1;
    i64 free = -1;
    i64 largestPiece = -1;
};
HeapUse heapUse(bool findLargest);
/// The last times there wasn't room for something, how much it was and where it was asked for, for the log, and forgets them. Empty where
/// that isn't kept.
std::string refusedAllocations();
/// Notes that there wasn't room for this much, from where it's called.
void noteRefusalForLog(size_t size);
/// Makes sure what was written is really on the card.
void commit(const std::string& path);

class Mutex {
public:
    Mutex();
    ~Mutex();
    Mutex(const Mutex&) = delete;
    void lock();
    void unlock();
    /// Locked only if it can be straight away.
    bool tryLock();

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
