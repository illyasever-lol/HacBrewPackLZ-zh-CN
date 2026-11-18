// Game files on a PC, read over the network or a USB cable. The PC version, run with `send`, shares them, and they show up here as a folder like
// any other, at pc:<name>/, so that installing from them works the same as from the SD card. Only reading is possible.
#pragma once

#include <atomic>
#include <functional>
#include <memory>

#include "sys.h"

namespace hbp::remote {

/// A way through to the PC. Each call goes all the way, or throws.
class Transport {
public:
    virtual ~Transport() = default;
    virtual void send(const void* data, size_t size) = 0;
    virtual void receive(void* data, size_t size) = 0;
    /// The USB cable has the one way through, which every request takes turns on, so nothing can be read beside it on a second connection.
    virtual bool onePipe() const { return false; }
};

/// The cable has one pair of endpoints and one shared 1 MiB buffer. Posting two transfers at
/// once - an install's answer and a probe for the sending service - makes the console's usb
/// system module fatal (2162-0002 / 0x4a2). One whole request/answer pair, or one whole probe,
/// holds this for as long as it touches the cable.
extern std::atomic<bool> cableBusy;

class CableGuard {
public:
    /// `enabled` is false for a network link. With `wait` false, a probe that finds the cable in
    /// use gives up instead of holding up an install; `held()` says whether it went ahead.
    CableGuard(bool enabled, bool wait);
    ~CableGuard();
    CableGuard(const CableGuard&) = delete;
    bool held() const { return _held; }

private:
    bool _held = false;
};

/// A PC that can be reached, and how.
struct Peer {
    /// What its folder is called, after pc:.
    std::string id;
    /// What it's shown as.
    std::string name;
    std::function<std::unique_ptr<Transport>()> connect;
    /// The PC's own name, the same however it's reached. Empty when it hasn't said.
    std::string machine;
};

/// The PCs there are just now. Each platform says how it finds them.
/// With the cable left out, the console's USB port isn't touched.
std::vector<Peer> peers(bool cable = true);
/// A PC on the cable that's also heard over the network is only kept on one of them, so its games are listed once: the cable, unless the
/// settings put Wi-Fi first.
void keepOneWayEach(std::vector<Peer>& peers);
/// Which way a PC on both is read through: the cable to begin with, as a cable is faster and steadier than Wi-Fi. A PC heard over the
/// network first, before the cable's PC has said its name, is read through the cable too once it has, when the cable comes first.
void setWifiFirst(bool wifiFirst);
bool wifiFirst();
/// Connections made the way the settings had it are let go, so the next request goes the way they say now.
void forgetLinks();
/// Something has come or gone since this was last asked.
bool peersChanged();
/// The app is closing: whatever's been looking for PCs stops, before what it uses is taken down.
void stopLooking();

/// Whatever this thread is waiting on a PC for is called off as soon as this says so, with Cancelled, as a list that's coming up gives way
/// to what's been asked for. Each platform's ways through look at it while they wait.
void setGiveWay(std::function<bool()> check);
std::function<bool()> giveWay();
bool shouldGiveWay();

constexpr u16 TcpPort = 25870;
constexpr u16 BeaconPort = 25871;
/// What a PC sends to say it's there: this, then the port it listens on and its name.
constexpr const char* BeaconMagic = "HBPZ1";

bool isRemote(const std::string& path);

class RemoteFile {
public:
    RemoteFile(std::string path, i64 size);
    ~RemoteFile();
    i64 size() const { return _size; }
    void read(i64 offset, void* buffer, size_t count);

private:
    /// What's being read from front to back comes in ahead of being asked for, on a connection of its own, while what came before is
    /// being worked on. Each request otherwise has to wait for the one before to come all the way across.
    struct Stream;
    bool fromStream(i64& offset, u8*& out, size_t& count);
    /// What came across from the PC, and how long that took, for the log: how fast the way through was, as against how fast the file was
    /// worked through.
    struct Tally {
        std::atomic<i64> bytes{0};
        std::atomic<i64> micros{0};
    };
    Tally _tally;
    i64 _given = 0;
    double _firstAt = 0, _lastAt = 0;
    bool _overCable = false;

    std::string _path;
    i64 _size;
    sys::Mutex _mutex;
    Bytes _block;
    i64 _blockStart = -1;
    std::unique_ptr<Stream> _stream;
    i64 _lastEnd = -1;
    int _inARow = 0;
};

std::shared_ptr<RemoteFile> open(const std::string& path);
/// A folder the PC couldn't list (as against one that isn't there) says so through failed, when that's given, as sys::list does.
std::vector<sys::DirEntry> list(const std::string& path, bool* failed = nullptr);
/// Negative if it isn't there.
i64 fileSize(const std::string& path);
bool isDirectory(const std::string& path);
/// Whether there's a folder there, with the PC failing to answer as can't tell, as sys::folderPresence does.
sys::Presence folderPresence(const std::string& path);

/// What the PC worked out itself about a game file or an extracted game's folder: its titles, names and icons, as lines and then the
/// icons. False if it can't say, such as a version from before it could, and it's to be read here instead. Working it out can take a
/// while, so it's asked again until it's done, calling `waiting` in between, which can throw to give up.
bool describe(const std::string& path, Bytes& answer, const std::function<void()>& waiting);
/// The same for what a game comes to compressed: `settings` is its first line, and `paths` its files, all on the one PC.
bool estimate(const std::vector<std::string>& paths, const std::string& settings, std::string& answer, const std::function<void()>& waiting);

}  // namespace hbp::remote
