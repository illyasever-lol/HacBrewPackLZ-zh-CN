#include "remote.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>

namespace hbp::remote {

std::atomic<bool> cableBusy{false};

CableGuard::CableGuard(bool enabled, bool wait) {
    if (!enabled)
        return;
    while (true) {
        bool expected = false;
        if (cableBusy.compare_exchange_strong(expected, true, std::memory_order_acquire, std::memory_order_relaxed)) {
            _held = true;
            return;
        }
        if (!wait)
            return;
        sys::sleepMs(1);
    }
}

CableGuard::~CableGuard() {
    if (_held)
        cableBusy.store(false, std::memory_order_release);
}

namespace {

// Each request is a header and a path, and each answer a header and what was asked for.
constexpr char RequestMagic[4] = {'H', 'B', 'P', 'Q'};
constexpr char AnswerMagic[4] = {'H', 'B', 'P', 'R'};
enum Command : u32 { Hello = 1, List = 2, Stat = 3, Read = 4, Describe = 5, Estimate = 6 };
/// Working: the PC is on it, and it's to be asked again.
enum Status : u32 { Done = 0, NotThere = 1, Failed = 2, Working = 3 };
/// How often to ask again while the PC works something out, and for how long at most.
constexpr int AskAgainMs = 300;
constexpr double LongestWait = 30 * 60;
constexpr size_t RequestHeader = 4 + 4 + 8 + 8 + 4;
constexpr size_t AnswerHeader = 4 + 4 + 8;
/// Small reads are what parsing a game file takes, and each one would otherwise go all the way to the PC and back.
constexpr size_t BlockSize = 1 << 20;
constexpr size_t LargestRead = 8 << 20;
constexpr i64 LargestAnswer = 64LL << 20;
/// Streaming: how much each request asks for, how many are out at once, how many pieces are held at most, received or on their way, and
/// how much of a file has to be left for it to be worth it. Over the cable, with the one way through, one request is out at a time, and the
/// pieces are as large as the PC sends at once, which leaves as little as there can be of each request's going there and back.
constexpr i64 StreamPiece = 4 << 20;
constexpr i64 CablePiece = 8 << 20;
constexpr int StreamAhead = 2;
constexpr int StreamHeld = 4;
constexpr i64 StreamWorthIt = 8 << 20;
/// Files that came to at least this from a PC say in the log how fast they came.
constexpr i64 WorthLogging = 16 << 20;

std::atomic<bool> g_wifiFirst{false};

struct Link {
    std::function<std::unique_ptr<Transport>()> connect;
    std::unique_ptr<Transport> transport;
    sys::Mutex mutex;
    /// Over the network, a second connection streams files, and is kept for the next one. The cable only has the one way through.
    bool streams = false;
    sys::Mutex spareMutex;
    std::unique_ptr<Transport> spare;
};

sys::Mutex g_linksMutex;
std::map<std::string, std::shared_ptr<Link>> g_links;

/// pc:<id>/<what's inside>
bool split(const std::string& path, std::string& id, std::string& inside) {
    if (path.compare(0, 3, "pc:") != 0)
        return false;
    size_t slash = path.find('/', 3);
    id = path.substr(3, slash == std::string::npos ? std::string::npos : slash - 3);
    inside = slash == std::string::npos ? "" : path.substr(slash + 1);
    while (!inside.empty() && inside.back() == '/')
        inside.pop_back();
    return !id.empty();
}

thread_local std::function<bool()> t_giveWay;

std::shared_ptr<Link> linkFor(const std::string& id) {
    sys::Lock lock(g_linksMutex);
    auto found = g_links.find(id);
    if (found != g_links.end())
        return found->second;
    for (auto& peer : peers()) {
        if (peer.id == id) {
            auto link = std::make_shared<Link>();
            link->connect = peer.connect;
            link->streams = id != "USB";
            g_links[id] = link;
            return link;
        }
    }
    throw ToolError("已找不到该 PC，请确认其上的 hacbrewpacklz send 是否仍在运行。");
}

void sendRequest(Transport& transport, Command command, i64 a, i64 b, const std::string& inside) {
    u8 request[RequestHeader];
    memcpy(request, RequestMagic, 4);
    wr32(&request[4], command);
    wr64(&request[8], (u64)a);
    wr64(&request[16], (u64)b);
    wr32(&request[24], (u32)inside.size());
    // Each part on its own, as over USB the other end has to read exactly as much as each comes to.
    transport.send(request, sizeof(request));
    if (!inside.empty())
        transport.send(inside.data(), inside.size());
}

Status receiveAnswer(Transport& transport, Bytes& answer) {
    u8 header[AnswerHeader];
    transport.receive(header, sizeof(header));
    if (memcmp(header, AnswerMagic, 4) != 0)
        throw ToolError("PC 返回了非本工具的数据");
    u32 status = rd32(header + 4);
    i64 length = (i64)rd64(header + 8);
    if (length < 0 || length > LargestAnswer)
        throw ToolError("PC 返回的数据过大");
    answer.resize((size_t)length);
    if (length > 0)
        transport.receive(answer.data(), answer.size());
    return (Status)status;
}

Status ask(const std::string& path, Command command, i64 a, i64 b, Bytes& answer) {
    std::string id, inside;
    if (!split(path, id, inside))
        throw ToolError("'" + path + "' 不在 PC 上");
    auto link = linkFor(id);
    sys::Lock lock(link->mutex);
    // The cable's single endpoint pair and buffer are shared with the service probe. Hold them
    // for the whole request/answer pair, not just one send or one receive.
    CableGuard cable(!link->streams, true);

    // A connection that's gone stale gets one more go with a new one.
    for (int attempt = 0;; attempt++) {
        try {
            if (!link->transport)
                link->transport = link->connect();
            sendRequest(*link->transport, command, a, b, inside);
            return receiveAnswer(*link->transport, answer);
        } catch (const Cancelled&) {
            // Called off part way through, it's not known where it's got to, so the next request goes on a new connection.
            link->transport.reset();
            throw;
        } catch (const ToolError&) {
            link->transport.reset();
            if (attempt > 0)
                throw;
        }
    }
}

std::string textOf(const Bytes& bytes) { return std::string(bytes.begin(), bytes.end()); }

/// Asks until the PC has an answer. The connection's free in between, for reading files.
bool askUntilDone(const std::string& path, Command command, Bytes& answer, const std::function<void()>& waiting) {
    double started = sys::now();
    for (;;) {
        Status status = ask(path, command, 0, 0, answer);
        if (status == Done)
            return true;
        if (status != Working || sys::now() - started > LongestWait)
            return false;
        waiting();
        sys::sleepMs(AskAgainMs);
    }
}

}  // namespace

bool describe(const std::string& path, Bytes& answer, const std::function<void()>& waiting) {
    return askUntilDone(path, Describe, answer, waiting);
}

bool estimate(const std::vector<std::string>& paths, const std::string& settings, std::string& answer, const std::function<void()>& waiting) {
    // The request goes where a path would, after the PC's own part of the first path.
    std::string id, inside, request = settings;
    for (auto& path : paths) {
        std::string thisId;
        if (!split(path, thisId, inside) || (!id.empty() && thisId != id))
            return false;
        id = thisId;
        request += "\n" + inside;
    }
    if (id.empty())
        return false;
    Bytes bytes;
    if (!askUntilDone("pc:" + id + "/" + request, Estimate, bytes, waiting))
        return false;
    answer = textOf(bytes);
    return true;
}

void keepOneWayEach(std::vector<Peer>& peers) {
    auto cable = std::find_if(peers.begin(), peers.end(), [](const Peer& peer) { return peer.id == "USB"; });
    if (cable == peers.end() || cable->machine.empty())
        return;
    std::string machine = cable->machine;
    if (g_wifiFirst) {
        bool onNetwork = std::any_of(peers.begin(), peers.end(), [&](const Peer& peer) { return peer.id != "USB" && peer.machine == machine; });
        if (onNetwork)
            peers.erase(cable);
        return;
    }
    peers.erase(std::remove_if(peers.begin(), peers.end(), [&](const Peer& peer) { return peer.id != "USB" && peer.machine == machine; }), peers.end());
}

void setWifiFirst(bool wifiFirst) { g_wifiFirst = wifiFirst; }

bool wifiFirst() { return g_wifiFirst; }

void forgetLinks() {
    sys::Lock lock(g_linksMutex);
    g_links.clear();
}

bool isRemote(const std::string& path) { return path.compare(0, 3, "pc:") == 0; }

void setGiveWay(std::function<bool()> check) { t_giveWay = std::move(check); }

std::function<bool()> giveWay() { return t_giveWay; }

bool shouldGiveWay() { return t_giveWay && t_giveWay(); }

std::vector<sys::DirEntry> list(const std::string& path, bool* failed) {
    Bytes answer;
    std::vector<sys::DirEntry> entries;
    Status status = ask(path, List, 0, 0, answer);
    if (status != Done) {
        if (failed && status != NotThere)
            *failed = true;
        return entries;
    }
    std::string text = textOf(answer);
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        // D<tab>name, or F<tab>size<tab>name
        size_t tab = line.find('\t');
        if (tab == std::string::npos)
            continue;
        sys::DirEntry entry;
        if (line[0] == 'D') {
            entry.isDirectory = true;
            entry.name = line.substr(tab + 1);
        } else {
            size_t second = line.find('\t', tab + 1);
            if (second == std::string::npos)
                continue;
            entry.size = atoll(line.substr(tab + 1, second - tab - 1).c_str());
            entry.name = line.substr(second + 1);
        }
        if (!entry.name.empty())
            entries.push_back(entry);
    }
    return entries;
}

i64 fileSize(const std::string& path) {
    Bytes answer;
    if (ask(path, Stat, 0, 0, answer) != Done)
        return -1;
    std::string text = textOf(answer);
    return text.size() > 2 && text[0] == 'F' ? atoll(text.c_str() + 2) : -1;
}

bool isDirectory(const std::string& path) {
    Bytes answer;
    return ask(path, Stat, 0, 0, answer) == Done && !answer.empty() && answer[0] == 'D';
}

sys::Presence folderPresence(const std::string& path) {
    Bytes answer;
    Status status = ask(path, Stat, 0, 0, answer);
    if (status == Done)
        return !answer.empty() && answer[0] == 'D' ? sys::Presence::There : sys::Presence::Missing;
    return status == NotThere ? sys::Presence::Missing : sys::Presence::CantTell;
}

std::shared_ptr<RemoteFile> open(const std::string& path) {
    i64 size = fileSize(path);
    if (size < 0)
        throw ToolError("无法打开 '" + path + "'（位于 PC 上）");
    return std::make_shared<RemoteFile>(path, size);
}

/// Asks for what comes next, a few pieces ahead, and takes in the answers as they come, on a thread of its own, so that the network
/// never waits on what's being done with what came before.
struct RemoteFile::Stream {
    std::shared_ptr<Link> link;
    /// Null over the cable, where each piece is asked for on the link's own way through, between whatever else is asked.
    std::unique_ptr<Transport> transport;
    std::string inside;
    i64 size;
    i64 piece;
    RemoteFile::Tally& tally;
    sys::Mutex mutex;
    sys::CondVar changed;
    /// What's come in, in order, each with where it starts.
    std::deque<std::pair<i64, Bytes>> pieces;
    /// Where the next request starts, and where what's come in so far ends.
    i64 asked, arrived;
    int out = 0;
    bool stop = false, failed = false;
    sys::Thread thread;

    Stream(std::shared_ptr<Link> link_, std::unique_ptr<Transport> transport_, std::string inside_, i64 size_, i64 from, RemoteFile::Tally& tally_)
        : link(std::move(link_)), transport(std::move(transport_)), inside(std::move(inside_)), size(size_), piece(transport ? StreamPiece : CablePiece),
          tally(tally_), asked(from), arrived(from) {
        if (transport)
            thread.start([this] { run(); }, -1, 1 << 16, true);
        else
            thread.start([this] { runTakingTurns(); }, -1, 1 << 16, true);
    }

    ~Stream() {
        {
            sys::Lock lock(mutex);
            stop = true;
            changed.wakeAll();
        }
        thread.join();
        // What was asked for has all come in by now, so the connection's ready for the next file.
        if (!failed && transport) {
            sys::Lock lock(link->spareMutex);
            link->spare = std::move(transport);
        }
    }

    void run() {
        try {
            for (;;) {
                i64 offset = 0, length = 0;
                {
                    sys::Lock lock(mutex);
                    while (!stop && out == 0 && !(asked < size && (int)pieces.size() < StreamHeld))
                        changed.wait(mutex);
                    if (stop && out == 0)
                        return;
                    if (!stop && out < StreamAhead && asked < size && (int)pieces.size() + out < StreamHeld) {
                        offset = asked;
                        length = std::min(StreamPiece, size - asked);
                        asked += length;
                        out++;
                    }
                }
                if (length > 0) {
                    sendRequest(*transport, Read, offset, length, inside);
                    continue;
                }
                // Answers come in the order they were asked for. Once stopping, they're still taken in, to leave the connection clear.
                Bytes got;
                i64 expected;
                {
                    sys::Lock lock(mutex);
                    expected = std::min(StreamPiece, size - arrived);
                }
                double started = sys::now();
                if (receiveAnswer(*transport, got) != Done || (i64)got.size() != expected)
                    throw ToolError("无法从 PC 读取文件");
                tally.bytes += expected;
                tally.micros += (i64)((sys::now() - started) * 1e6);
                sys::Lock lock(mutex);
                pieces.emplace_back(arrived, std::move(got));
                arrived += expected;
                out--;
                changed.wakeAll();
            }
        } catch (const ToolError&) {
            sys::Lock lock(mutex);
            failed = true;
            changed.wakeAll();
        }
    }

    /// Over the cable: the next piece is asked for while the one before is worked on, which is all that can be had with one way through, as
    /// the PC answers each request before it reads the next. Anything else asked meanwhile, such as another file of the game, goes between.
    void runTakingTurns() {
        try {
            for (;;) {
                i64 offset = 0, length = 0;
                {
                    sys::Lock lock(mutex);
                    while (!stop && !(asked < size && (int)pieces.size() < StreamHeld))
                        changed.wait(mutex);
                    if (stop)
                        return;
                    offset = asked;
                    length = std::min(piece, size - asked);
                    asked += length;
                }
                Bytes got;
                double started = sys::now();
                {
                    sys::Lock lock(link->mutex);
                    try {
                        if (!link->transport)
                            link->transport = link->connect();
                        sendRequest(*link->transport, Read, offset, length, inside);
                        if (receiveAnswer(*link->transport, got) != Done || (i64)got.size() != length)
                            throw ToolError("无法从 PC 读取文件");
                    } catch (...) {
                        // Where it got to isn't known, so whatever's asked next starts again.
                        link->transport.reset();
                        throw;
                    }
                }
                tally.bytes += length;
                tally.micros += (i64)((sys::now() - started) * 1e6);
                sys::Lock lock(mutex);
                pieces.emplace_back(offset, std::move(got));
                arrived = offset + length;
                changed.wakeAll();
            }
        } catch (const ToolError&) {
            sys::Lock lock(mutex);
            failed = true;
            changed.wakeAll();
        } catch (const Cancelled&) {
            sys::Lock lock(mutex);
            failed = true;
            changed.wakeAll();
        }
    }
};

RemoteFile::RemoteFile(std::string path, i64 size) : _path(std::move(path)), _size(size) {}

/// How it came, for the log, which is what tells a slow cable from a slow card: how much, over what, how fast the way through gave it, and
/// how fast it was worked through.
RemoteFile::~RemoteFile() {
    _stream.reset();
    if (_given < WorthLogging)
        return;
    std::string id, inside;
    split(_path, id, inside);
    size_t slash = inside.rfind('/');
    std::string name = slash == std::string::npos ? inside : inside.substr(slash + 1);
    double seconds = std::max(_lastAt - _firstAt, 0.001);
    double linkSeconds = std::max((double)_tally.micros / 1e6, 0.001);
    std::string from = id == "USB" ? std::string("the PC on the USB cable") : _overCable ? id + ", through the USB cable" : id + ", over the network";
    sink().message(format("Read %s of %s from %s in %.0f s, %.1f MB/s, with the PC sending at %.1f MB/s", formatSize(_given).c_str(), name.c_str(), from.c_str(),
                          seconds, (double)_given / seconds / (1 << 20), (double)_tally.bytes / linkSeconds / (1 << 20)));
}

/// Takes what it can from what's been streamed, waiting for it if it's on its way. False if it's stopped short: what's left is somewhere
/// the stream isn't, or the stream's broken, and the stream's dropped if it can't be any more use.
bool RemoteFile::fromStream(i64& offset, u8*& out, size_t& count) {
    Stream& stream = *_stream;
    bool keep = true;
    {
        sys::Lock lock(stream.mutex);
        while (count > 0) {
            // A piece behind the one before what's wanted is done with. That one's kept, for a reader just behind another.
            while (stream.pieces.size() >= 2 && stream.pieces[1].first + (i64)stream.pieces[1].second.size() <= offset) {
                stream.pieces.pop_front();
                stream.changed.wakeAll();
            }
            bool found = false;
            for (auto& piece : stream.pieces) {
                if (offset >= piece.first && offset < piece.first + (i64)piece.second.size()) {
                    size_t n = (size_t)std::min<i64>((i64)count, piece.first + (i64)piece.second.size() - offset);
                    memcpy(out, piece.second.data() + (offset - piece.first), n);
                    out += n;
                    offset += (i64)n;
                    count -= n;
                    found = true;
                    break;
                }
            }
            if (found)
                continue;
            i64 held = stream.pieces.empty() ? stream.arrived : stream.pieces.front().first;
            if (stream.failed || offset >= stream.arrived + StreamHeld * stream.piece) {
                keep = false;
                break;
            }
            // Behind the stream, which carries on for whatever comes after.
            if (offset < held)
                break;
            stream.changed.wait(stream.mutex);
        }
    }
    if (!keep)
        _stream.reset();
    return count == 0;
}

void RemoteFile::read(i64 offset, void* buffer, size_t count) {
    if (offset < 0 || offset + (i64)count > _size)
        throw ToolError("内部错误：读取超出 PC 上文件的范围");
    u8* out = (u8*)buffer;
    sys::Lock lock(_mutex);
    double now = sys::now();
    if (_given == 0)
        _firstAt = now;
    _lastAt = now;
    _given += (i64)count;

    // Read from front to back for a while, a file gets streamed from there on: over the network on a second connection, and over the cable
    // a piece ahead on the one way through.
    _inARow = offset == _lastEnd ? _inARow + 1 : 0;
    _lastEnd = offset + (i64)count;
    if (!_stream && _inARow >= 2 && _size - offset >= StreamWorthIt) {
        try {
            std::string id, inside;
            split(_path, id, inside);
            auto link = linkFor(id);
            bool cable;
            {
                sys::Lock linkLock(link->mutex);
                cable = link->transport ? link->transport->onePipe() : !link->streams;
            }
            _overCable = cable;
            if (cable) {
                _stream = std::make_unique<Stream>(link, nullptr, inside, _size, offset, _tally);
            } else {
                std::unique_ptr<Transport> transport;
                {
                    sys::Lock spare(link->spareMutex);
                    transport = std::move(link->spare);
                }
                if (!transport)
                    transport = link->connect();
                // A PC heard over the network that turned out to be on the cable is read through the cable, the one way there is.
                if (transport->onePipe()) {
                    _overCable = true;
                    transport.reset();
                    _stream = std::make_unique<Stream>(link, nullptr, inside, _size, offset, _tally);
                } else {
                    _stream = std::make_unique<Stream>(link, std::move(transport), inside, _size, offset, _tally);
                }
            }
        } catch (const ToolError&) {
            // Then it's read the way it was, which works just as well, only slower.
        }
    }
    if (_stream && fromStream(offset, out, count))
        return;

    while (count > 0) {
        if (_blockStart >= 0 && offset >= _blockStart && offset < _blockStart + (i64)_block.size()) {
            size_t n = std::min(count, (size_t)(_blockStart + (i64)_block.size() - offset));
            memcpy(out, _block.data() + (offset - _blockStart), n);
            out += n;
            offset += (i64)n;
            count -= n;
            continue;
        }
        Bytes answer;
        double started = sys::now();
        if (count >= BlockSize) {
            size_t n = std::min(count, LargestRead);
            if (ask(_path, Read, offset, (i64)n, answer) != Done || answer.size() != n)
                throw ToolError("无法读取 '" + _path + "'（来自 PC）");
            _tally.bytes += (i64)n;
            _tally.micros += (i64)((sys::now() - started) * 1e6);
            memcpy(out, answer.data(), n);
            out += n;
            offset += (i64)n;
            count -= n;
            continue;
        }
        i64 start = offset / (i64)BlockSize * (i64)BlockSize;
        size_t n = (size_t)std::min<i64>((i64)BlockSize, _size - start);
        if (ask(_path, Read, start, (i64)n, answer) != Done || answer.size() != n)
            throw ToolError("无法读取 '" + _path + "'（来自 PC）");
        _tally.bytes += (i64)n;
        _tally.micros += (i64)((sys::now() - started) * 1e6);
        _block = std::move(answer);
        _blockStart = start;
    }
}

}  // namespace hbp::remote
