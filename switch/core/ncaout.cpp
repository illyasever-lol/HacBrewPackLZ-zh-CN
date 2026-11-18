#include "ncaout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <exception>
#include <map>
#include <new>
#include <unordered_map>

#include "lz4.h"
#include "remote.h"

namespace hbp {

// -------------------------------------------------------------------------------------------------------------------
// The hash tree

namespace {
constexpr int HashSize = 0x20;
constexpr int HashLevelCount = 5;
}  // namespace

IvfcBuilder::IvfcBuilder() : _partial(BlockSize) { _hashes.reserve(0x10000); }

void IvfcBuilder::expect(i64 dataBytes) {
    constexpr i64 ForTheUpperLevels = BlockSize * 16;
    _hashes.reserve((size_t)(dataBytes / BlockSize * HashSize + ForTheUpperLevels));
}

void IvfcBuilder::add(const u8* buffer, size_t count) {
    if (_partialLength > 0) {
        size_t n = std::min(count, (size_t)BlockSize - _partialLength);
        memcpy(_partial.data() + _partialLength, buffer, n);
        _partialLength += n;
        buffer += n;
        count -= n;
        if (_partialLength < BlockSize)
            return;
        hashBlocks(_partial.data(), 1);
        _partialLength = 0;
    }

    size_t blocks = count / BlockSize;
    if (blocks > 0) {
        hashBlocks(buffer, blocks);
        buffer += blocks * BlockSize;
        count -= blocks * BlockSize;
    }

    if (count > 0) {
        memcpy(_partial.data(), buffer, count);
        _partialLength = count;
    }
}

void IvfcBuilder::hashBlocks(const u8* buffer, size_t blocks) {
    size_t start = _hashes.size();
    _hashes.resize(start + blocks * HashSize);
    for (size_t i = 0; i < blocks; i++)
        sha256(buffer + i * BlockSize, BlockSize, _hashes.data() + start + i * HashSize);
}

static Bytes padLevel(const u8* hashes, size_t size) {
    Bytes level(size + (size_t)IvfcBuilder::padSize((i64)size));
    if (size > 0)
        memcpy(level.data(), hashes, size);
    return level;
}

IvfcResult IvfcBuilder::finish(i64 dataSize, bool dataFirst) {
    if (_partialLength != 0)
        throw ToolError("内部错误：IVFC 数据必须按整块输入");

    // The last level is what takes up the room, a good 300 MB for a very large game, so it stays where it is.
    _hashes.resize(_hashes.size() + (size_t)padSize((i64)_hashes.size()));
    Bytes upper[HashLevelCount - 1];
    const Bytes* below = &_hashes;
    for (int i = HashLevelCount - 2; i >= 0; i--) {
        size_t blocks = below->size() / BlockSize;
        Bytes hashes(blocks * HashSize);
        for (size_t b = 0; b < blocks; b++)
            sha256(below->data() + b * BlockSize, BlockSize, hashes.data() + b * HashSize);
        upper[i] = padLevel(hashes.data(), hashes.size());
        below = &upper[i];
    }

    IvfcResult result;
    i64 start = dataFirst ? dataSize : 0;
    i64 position = 0;
    for (int i = 0; i < HashLevelCount; i++) {
        i64 size = (i64)(i == HashLevelCount - 1 ? _hashes.size() : upper[i].size());
        result.levelOffsets[i] = start + position;
        result.levelSizes[i] = size;
        position += size;
    }
    result.levelOffsets[HashLevelCount] = dataFirst ? 0 : position;
    result.levelSizes[HashLevelCount] = dataSize;

    sha256(upper[0].data(), upper[0].size(), result.masterHash);
    _hashes.insert(_hashes.begin(), (size_t)position - _hashes.size(), 0);
    for (int i = 0; i < HashLevelCount - 1; i++)
        memcpy(_hashes.data() + result.levelOffsets[i] - start, upper[i].data(), upper[i].size());
    result.hashLevels = std::move(_hashes);
    return result;
}

i64 memoryToRebuild(i64 imageBytes, int blockSize) {
    // Each block also has to be remembered, so what comes after it can point at it if it holds the same, and where it was stored.
    constexpr i64 ToFindItAgain = 160;
    constexpr i64 PerBlock = sizeof(BucketTree::Entry) + BucketTree::EntrySize + ToFindItAgain;
    return imageBytes / IvfcBuilder::BlockSize * 2 * HashSize + imageBytes / blockSize * PerBlock;
}

i64 IvfcBuilder::hashLevelsSize(i64 dataSize) {
    i64 total = 0, level = dataSize;
    for (int i = 0; i < HashLevelCount; i++) {
        i64 hashes = (level + BlockSize - 1) / BlockSize * HashSize;
        level = hashes + padSize(hashes);
        total += level;
    }
    return total;
}

// -------------------------------------------------------------------------------------------------------------------
// Workers

namespace {

/// Holds whichever failure came first, so it can be raised where someone can deal with it.
class Failure {
public:
    template <typename Work>
    void guard(Work&& work) {
        try {
            work();
        } catch (...) {
            sys::Lock lock(_mutex);
            if (!_error)
                _error = std::current_exception();
        }
    }
    bool failed() {
        sys::Lock lock(_mutex);
        return (bool)_error;
    }
    void raise() {
        sys::Lock lock(_mutex);
        if (_error)
            std::rethrow_exception(_error);
    }

private:
    sys::Mutex _mutex;
    std::exception_ptr _error;
};

}  // namespace

void parallelFor(int threads, int count, const std::function<void(int, int)>& work) {
    if (count <= 0)
        return;
    int workers = std::min(threads, count);
    if (workers <= 1) {
        for (int i = 0; i < count; i++)
            work(i, 0);
        return;
    }

    std::atomic<int> next{0};
    Failure failure;
    // The threads that help with it give way whenever the one that asked for it would.
    std::function<bool()> giveWay = remote::giveWay();
    auto body = [&](int worker) {
        if (worker > 0)
            remote::setGiveWay(giveWay);
        failure.guard([&] {
            while (true) {
                while (sys::workerShouldWait(worker) && next.load() < count && !failure.failed())
                    sys::sleepMs(20);
                int i = next.fetch_add(1);
                if (i >= count || failure.failed())
                    return;
                work(i, worker);
            }
        });
    };

    std::vector<std::unique_ptr<sys::Thread>> pool;
    for (int w = 1; w < workers; w++) {
        pool.emplace_back(new sys::Thread);
        pool.back()->start([&body, w] { body(w); }, sys::workerCore(w), 1 << 18);
    }
    body(0);
    for (auto& thread : pool)
        thread->join();
    failure.raise();
}

// -------------------------------------------------------------------------------------------------------------------
// Compressing a RomFS

namespace RomFsCompressor {

namespace {

constexpr int PhysicalAlignment = 0x10;
constexpr int MaxStoredRun = 0x4000000;
constexpr int MinimumSaving = 64;
constexpr int BatchCount = 4;

using PieceHash = std::array<u64, 2>;

struct Batch {
    Batch(size_t capacity, size_t blocks) : input(new u8[capacity]), output(new u8[capacity]), outputLength(blocks), hashes(blocks) { this->blocks.reserve(blocks); }
    std::unique_ptr<u8[]> input;
    std::unique_ptr<u8[]> output;
    std::vector<int> outputLength;
    std::vector<PieceHash> hashes;
    /// The blocks of the plan it holds.
    std::vector<SharedBlock> blocks;
    i64 virtualOffset = 0;
    size_t bytes = 0;
};

/// Where a shared piece was stored, and what it held.
struct StoredPiece {
    i64 physical = 0;
    int physicalSize = 0;
    bool compressed = false;
    bool known = false;
    PieceHash hash{};
};

/// The compressed size, or 0 if it's to be stored as it is.
int compressOne(Lz4Encoder& encoder, u8* check, const u8* input, int length, u8* output, int level, i64 virtualOffset) {
    int compressed = encoder.compress(input, length, output, compressCapacity(length), level);
    if (compressed > 0) {
        int decoded = lz4Decode(output, compressed, check, Lz4Encoder::MaxBlockSize);
        if (decoded != length || memcmp(check, input, (size_t)length) != 0)
            throw ToolError(format("Internal error: LZ4 round trip failed at RomFS offset 0x%llx", (long long)virtualOffset));
    }
    return std::max(compressed, 0);
}

using BatchQueue = Handover<Batch>;

class PayloadWriter {
public:
    PayloadWriter(ByteSink& payload, i64 imageBytes, int blockSize) : _payload(payload) {
        _ivfc.expect(imageBytes);
        _entries.reserve((size_t)(imageBytes / blockSize + 1));
    }

    i64 position() const { return _position; }

    /// Returns where it's stored.
    i64 addCompressed(i64 virtualOffset, const u8* buffer, int length) {
        align(PhysicalAlignment);
        i64 physical = _position;
        _entries.push_back(BucketTree::Entry{virtualOffset, physical, 3, length});
        write(buffer, (size_t)length);
        _storedRunEnd = -1;
        return physical;
    }

    /// A piece stored before, held here too, found while compressing or through the update.
    void addReference(i64 virtualOffset, const StoredPiece& piece, int length, bool found) {
        _sharedBytes += length;
        _foundBytes += found ? length : 0;
        if (piece.compressed) {
            _entries.push_back(BucketTree::Entry{virtualOffset, piece.physical, 3, piece.physicalSize});
            _storedRunEnd = -1;
            return;
        }
        // Stored as it is, it carries on a run of stored bytes that ends where it starts, as anything stored does.
        if (_storedRunEnd == piece.physical && _storedRunSize + length <= MaxStoredRun) {
            _storedRunSize += length;
        } else {
            _entries.push_back(BucketTree::Entry{virtualOffset, piece.physical, 0, -1});
            _storedRunSize = length;
        }
        _storedRunEnd = piece.physical + length;
    }

    /// Returns where it's stored.
    i64 addStored(i64 virtualOffset, const u8* buffer, int length) {
        // Consecutive stored blocks share one entry: raw entries are random accessible and have no size limit. As in retail NCAs,
        // a raw entry's physical size is -1, because the firmware never reads it.
        if (_storedRunEnd == _position && _storedRunSize + length <= MaxStoredRun) {
            _storedRunSize += length;
        } else {
            align(PhysicalAlignment);
            _entries.push_back(BucketTree::Entry{virtualOffset, _position, 0, -1});
            _storedRunSize = length;
        }
        i64 physical = _position;
        write(buffer, (size_t)length);
        _storedRunEnd = _position;
        return physical;
    }

    CompressedData finish(i64 virtualSize) {
        align(IvfcBuilder::BlockSize);
        i64 tableOffset = _position;
        Bytes table = BucketTree::build(_entries, virtualSize);
        write(table.data(), table.size());

        CompressedData data;
        data.virtualSize = virtualSize;
        data.tableOffset = tableOffset;
        data.tableSize = (i64)table.size();
        data.dataSize = _position;
        data.entryCount = (int)_entries.size();
        data.sharedBytes = _sharedBytes;
        data.foundBytes = _foundBytes;
        data.ivfc = _ivfc.finish(_position, true);
        return data;
    }

private:
    void align(int alignment) {
        static const u8 zeros[IvfcBuilder::BlockSize] = {};
        int padding = (int)(-_position & (alignment - 1));
        if (padding > 0)
            write(zeros, (size_t)padding);
    }

    void write(const u8* buffer, size_t length) {
        _ivfc.add(buffer, length);
        _position += (i64)length;
        _payload.write(buffer, length);
    }

    ByteSink& _payload;
    IvfcBuilder _ivfc;
    std::vector<BucketTree::Entry> _entries;
    i64 _position = 0;
    i64 _storedRunEnd = -1;
    i64 _storedRunSize = 0;
    i64 _sharedBytes = 0;
    i64 _foundBytes = 0;
};

/// Where the update's map has a piece of the original that's held more than once.
struct Place {
    i64 image;
    int piece;
    bool first;
};

/// Every place the update's map has a piece of the original that's held more than once, in order, and how many pieces that is.
std::vector<Place> mapPlaces(i64 imageLength, int blockSize, const std::vector<KeptRange>* kept, int& pieces) {
    // Every whole piece of the original that the image holds, and where. They can't overlap, as the image only holds one thing in each
    // place, but anything that would is left out rather than trusted.
    std::vector<std::pair<i64, i64>> places;
    if (kept) {
        for (auto& range : *kept) {
            i64 first = (range.original + blockSize - 1) / blockSize * blockSize;
            for (i64 piece = first; piece + blockSize <= range.original + range.length; piece += blockSize) {
                i64 image = range.image + (piece - range.original);
                if (image >= 0 && image + blockSize <= imageLength)
                    places.emplace_back(image, piece);
            }
        }
    }
    std::sort(places.begin(), places.end());
    std::vector<std::pair<i64, i64>> apart;
    for (auto& place : places) {
        if (apart.empty() || place.first >= apart.back().first + blockSize)
            apart.push_back(place);
    }
    std::map<i64, int> held;
    for (auto& place : apart)
        held[place.second]++;

    // Pieces are numbered as they're first come to.
    std::map<i64, int> numbers;
    std::vector<Place> shared;
    for (auto& place : apart) {
        if (held[place.second] < 2)
            continue;
        auto known = numbers.find(place.second);
        bool first = known == numbers.end();
        int number = first ? (int)numbers.size() : known->second;
        if (first)
            numbers[place.second] = number;
        shared.push_back(Place{place.first, number, first});
    }
    pieces = (int)numbers.size();
    return shared;
}

u64 splitMix(u64& state) {
    u64 z = state += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/// A fixed value for each byte, the same in every copy of the program. 64 bytes of one value come to that value's negative, which is never let
/// be a spot, however small the blocks, so zeros and other padding have none.
const u64* gear() {
    static const std::array<u64, 256> table = [] {
        std::array<u64, 256> values{};
        u64 state = 0x2545F4914F6CDD1DULL;
        for (auto& value : values) {
            do {
                value = splitMix(state);
            } while (((0 - value) >> 56) == 0);
        }
        return values;
    }();
    return table.data();
}

/// Plans the blocks as the image is read, in order, and finds on the way what the game holds again by itself, as SharedPieces.Planner does
/// on the PC, which says how. Every place the update's map gives is a block of its own, as planShared has it. Between them, a block's worth
/// that's the same as a whole block stored before points at that block instead of being stored again, wherever it is, and the blocks go on
/// from after it.
class Planner {
public:
    Planner(const SourcePtr& image, int blockSize, const std::vector<KeptRange>* kept)
        : _image(image), _length(image->length()), _blockSize(blockSize) {
        int bits = 0;
        while ((1 << bits) < blockSize)
            bits++;
        // A spot every 1/16 of a block on average.
        _shift = 64 - (bits - 4);
        _map = mapPlaces(_length, blockSize, kept, _pieces);
        // The two blocks looked through, and a read: it only reads while it has less than those.
        _bufferSize = 2 * (size_t)blockSize + ReadSize;
        _buffer.reset(new u8[_bufferSize]);
        // Nearly every block is remembered, and having the room from the start never takes twice it (memoryToRebuild counts on that).
        _targets.reserve((size_t)(_length / blockSize) + 1);
        _keys.reserve((size_t)(_length / blockSize) + 1);
    }

    /// The highest piece number there can be, plus one.
    int pieceCount() const { return _pieces + (int)(_length / _blockSize) + 1; }

    /// The next blocks, up to most, with their bytes copied to the start of input. False once there are none left.
    bool next(u8* input, size_t most, std::vector<SharedBlock>& blocks, i64& start, size_t& bytes) {
        blocks.clear();
        start = !_planned.empty() ? _planned.front().offset : _position;
        bytes = 0;
        while (blocks.size() < most && (!_planned.empty() || _position < _length)) {
            if (_planned.empty())
                step();
            // Copied as it's planned, so the buffer only has to hold what's looked through.
            const SharedBlock& block = _planned.front();
            memcpy(input + bytes, _buffer.get() + (block.offset - _bufferStart), (size_t)block.length);
            bytes += (size_t)block.length;
            blocks.push_back(block);
            _planned.pop_front();
        }
        return !blocks.empty();
    }

private:
    /// The bytes the rolling hash takes in: a spot depends on these alone, wherever they are.
    static constexpr int Span = 64;
    /// A key many blocks have is kept for the first ones, as many blocks can start alike.
    static constexpr int BlocksPerKey = 256;
    /// Words from across a block that a candidate has to have the same before all of it is hashed.
    static constexpr int SampleWords = 16;
    static constexpr int ReadSize = 1 << 22;

    struct Target {
        i64 offset = 0;
        int keyOffset = 0;
        /// -1 if the block has only the one spot.
        int secondOffset = -1;
        u64 secondValue = 0;
        u64 sample = 0;
        u64 hash = 0;
        int piece = -1;
        /// Where it was last looked for. Within a block of that, it isn't looked for again, which keeps data that repeats every few bytes
        /// from being hashed at every one.
        i64 lastTried = INT64_MIN / 2;
        /// The next block with the same key, or -1.
        int next = -1;
    };

    /// Plans the block at the position, or a block held again and what's before it.
    void step() {
        fill(_position, std::min(_position + 2 * (i64)_blockSize, _length));
        while (_firstSpot < _spotAt.size() && _spotAt[_firstSpot] < _position)
            _firstSpot++;
        if (_firstSpot >= 0x10000) {
            _spotAt.erase(_spotAt.begin(), _spotAt.begin() + (std::ptrdiff_t)_firstSpot);
            _spotValue.erase(_spotValue.begin(), _spotValue.begin() + (std::ptrdiff_t)_firstSpot);
            _firstSpot = 0;
        }

        i64 limit = _length;
        if (_nextMap < _map.size()) {
            const Place& place = _map[_nextMap];
            if (place.image == _position) {
                _planned.push_back(SharedBlock{_position, _blockSize, place.piece, place.first, false});
                if (place.first)
                    remember(_position, place.piece);
                _position += _blockSize;
                _nextMap++;
                return;
            }
            limit = place.image;
        }

        i64 at = 0;
        int piece = -1;
        if (limit - _position >= _blockSize && find(limit, at, piece)) {
            if (at > _position)
                _planned.push_back(SharedBlock{_position, (int)(at - _position), -1, false, false});
            _planned.push_back(SharedBlock{at, _blockSize, piece, false, true});
            _position = at + _blockSize;
            return;
        }

        int length = (int)std::min<i64>(_blockSize, limit - _position);
        bool remembered = length == _blockSize && remember(_position, _pieces);
        _planned.push_back(SharedBlock{_position, length, remembered ? _pieces : -1, remembered, false});
        if (remembered)
            _pieces++;
        _position += length;
    }

    /// The earliest place in the next block's worth that holds a whole block stored before.
    bool find(i64 limit, i64& at, int& piece) {
        i64 x = _position;
        i64 w = _blockSize;
        _candidates.clear();
        // A block's key is at least 63 bytes into it, and less than a block.
        for (size_t i = firstSpotFrom(x + Span - 1); i < _spotAt.size() && _spotAt[i] < x + 2 * w - 1; i++) {
            auto key = _keys.find(_spotValue[i]);
            if (key == _keys.end())
                continue;
            for (int t = key->second; t >= 0; t = _targets[(size_t)t].next) {
                const Target& target = _targets[(size_t)t];
                i64 q = _spotAt[i] - target.keyOffset;
                if (q >= x && q < x + w && q >= target.offset + w && q + w <= limit)
                    _candidates.emplace_back(q, t);
            }
        }
        std::sort(_candidates.begin(), _candidates.end());
        for (auto& candidate : _candidates) {
            i64 q = candidate.first;
            Target& target = _targets[(size_t)candidate.second];
            if (target.secondOffset >= 0 && !hasSpot(q + target.secondOffset, target.secondValue))
                continue;
            if (sample(q) != target.sample)
                continue;
            if (q < target.lastTried + w)
                continue;
            target.lastTried = q;
            if (blockHash(window(q), (size_t)_blockSize, 0) != target.hash)
                continue;
            at = q;
            piece = target.piece;
            return true;
        }
        return false;
    }

    /// Takes note of a whole block stored here, so later data can point at it. False if it has no spot, or its key has enough blocks.
    bool remember(i64 offset, int piece) {
        i64 end = offset + _blockSize;
        size_t i = firstSpotFrom(offset + Span - 1);
        if (i >= _spotAt.size() || _spotAt[i] >= end)
            return false;
        u64 key = _spotValue[i];
        int last = -1, count = 0;
        auto known = _keys.find(key);
        if (known != _keys.end()) {
            for (int t = known->second; t >= 0; t = _targets[(size_t)t].next) {
                last = t;
                count++;
            }
        }
        if (count >= BlocksPerKey)
            return false;
        bool second = i + 1 < _spotAt.size() && _spotAt[i + 1] < end;
        Target target;
        target.offset = offset;
        target.keyOffset = (int)(_spotAt[i] - offset);
        target.secondOffset = second ? (int)(_spotAt[i + 1] - offset) : -1;
        target.secondValue = second ? _spotValue[i + 1] : 0;
        target.sample = sample(offset);
        target.hash = blockHash(window(offset), (size_t)_blockSize, 0);
        target.piece = piece;
        _targets.push_back(target);
        int added = (int)_targets.size() - 1;
        if (last >= 0)
            _targets[(size_t)last].next = added;
        else
            _keys[key] = added;
        return true;
    }

    const u8* window(i64 offset) const { return _buffer.get() + (offset - _bufferStart); }

    /// A hash of a word from the middle of each 1/16 of the block here: far quicker than all of it, and blocks that only start alike differ.
    u64 sample(i64 offset) const {
        u8 words[SampleWords * 8];
        const u8* start = window(offset);
        int step = _blockSize / SampleWords;
        for (int k = 0; k < SampleWords; k++)
            memcpy(words + k * 8, start + k * step + step / 2, 8);
        return blockHash(words, sizeof(words), 3);
    }

    bool hasSpot(i64 at, u64 value) const {
        size_t i = firstSpotFrom(at);
        return i < _spotAt.size() && _spotAt[i] == at && _spotValue[i] == value;
    }

    size_t firstSpotFrom(i64 at) const {
        return (size_t)(std::lower_bound(_spotAt.begin() + (std::ptrdiff_t)_firstSpot, _spotAt.end(), at) - _spotAt.begin());
    }

    /// Reads on until the buffer holds the image up to need, keeping what's from keep on.
    void fill(i64 keep, i64 need) {
        while (_bufferStart + (i64)_bufferFill < need) {
            i64 end = _bufferStart + (i64)_bufferFill;
            size_t wanted = (size_t)std::min<i64>(ReadSize, _length - end);
            if (_bufferSize - _bufferFill < wanted) {
                size_t drop = (size_t)(keep - _bufferStart);
                memmove(_buffer.get(), _buffer.get() + drop, _bufferFill - drop);
                _bufferStart = keep;
                _bufferFill -= drop;
            }
            _image->read(end, _buffer.get() + _bufferFill, wanted);
            roll(_bufferFill, wanted, end);
            _bufferFill += wanted;
        }
    }

    /// The rolling hash over bytes just read, noting every spot.
    void roll(size_t offset, size_t count, i64 at) {
        const u64* values = gear();
        const u8* bytes = _buffer.get() + offset;
        u64 h = _rolling;
        int shift = _shift;
        for (size_t i = 0; i < count; i++) {
            h = (h << 1) + values[bytes[i]];
            if ((h >> shift) == 0) {
                _spotAt.push_back(at + (i64)i);
                _spotValue.push_back(h);
            }
        }
        _rolling = h;
    }

    SourcePtr _image;
    i64 _length;
    int _blockSize;
    int _shift = 52;
    std::vector<Place> _map;
    size_t _nextMap = 0;
    int _pieces = 0;
    i64 _position = 0;

    std::unique_ptr<u8[]> _buffer;
    size_t _bufferSize = 0;
    i64 _bufferStart = 0;
    size_t _bufferFill = 0;
    u64 _rolling = 0;

    std::vector<i64> _spotAt;
    std::vector<u64> _spotValue;
    size_t _firstSpot = 0;

    std::unordered_map<u64, int> _keys;
    std::vector<Target> _targets;
    std::deque<SharedBlock> _planned;
    std::vector<std::pair<i64, int>> _candidates;
};

}  // namespace

std::vector<u64> spotsIn(const u8* data, size_t length, int blockSize) {
    int bits = 0;
    while ((1 << bits) < blockSize)
        bits++;
    int shift = 64 - (bits - 4);
    const u64* values = gear();
    std::vector<u64> spots;
    u64 h = 0;
    for (size_t i = 0; i < length; i++) {
        h = (h << 1) + values[data[i]];
        if (i >= 63 && (h >> shift) == 0)
            spots.push_back(h);
    }
    std::sort(spots.begin(), spots.end());
    spots.erase(std::unique(spots.begin(), spots.end()), spots.end());
    return spots;
}

SharedPlan planShared(i64 imageLength, int blockSize, const std::vector<KeptRange>* kept) {
    int pieces = 0;
    std::vector<Place> shared = mapPlaces(imageLength, blockSize, kept, pieces);
    SharedPlan plan;
    plan.blocks.reserve((size_t)(imageLength / blockSize) + shared.size() * 2 + 1);
    size_t next = 0;
    for (i64 position = 0; position < imageLength;) {
        if (next < shared.size() && shared[next].image == position) {
            plan.blocks.push_back(SharedBlock{position, blockSize, shared[next].piece, shared[next].first});
            plan.references += shared[next].first ? 0 : 1;
            position += blockSize;
            next++;
            continue;
        }
        i64 limit = next < shared.size() ? shared[next].image : imageLength;
        int length = (int)std::min<i64>(blockSize, limit - position);
        plan.blocks.push_back(SharedBlock{position, length, -1, false});
        position += length;
    }
    plan.sharedBytes = (i64)plan.references * blockSize;
    return plan;
}

namespace {
constexpr u64 Prime1 = 0x9E3779B185EBCA87ULL;
constexpr u64 Prime2 = 0xC2B2AE3D27D4EB4FULL;
constexpr u64 Prime3 = 0x165667B19E3779F9ULL;
constexpr u64 Prime4 = 0x85EBCA77C2B2AE63ULL;
constexpr u64 Prime5 = 0x27D4EB2F165667C5ULL;

inline u64 rotateLeft(u64 value, int bits) { return (value << bits) | (value >> (64 - bits)); }
inline u64 word(const u8* at) {
    u64 value;
    memcpy(&value, at, 8);
    return value;
}
inline u64 hashRound(u64 accumulator, u64 input) { return rotateLeft(accumulator + input * Prime2, 31) * Prime1; }
inline u64 hashMerge(u64 h, u64 value) { return (h ^ hashRound(0, value)) * Prime1 + Prime4; }
}  // namespace

u64 blockHash(const u8* data, size_t length, u64 seed) {
    size_t position = 0;
    u64 h;
    if (length >= 32) {
        u64 v1 = seed + Prime1 + Prime2, v2 = seed + Prime2, v3 = seed, v4 = seed - Prime1;
        for (; position + 32 <= length; position += 32) {
            v1 = hashRound(v1, word(data + position));
            v2 = hashRound(v2, word(data + position + 8));
            v3 = hashRound(v3, word(data + position + 16));
            v4 = hashRound(v4, word(data + position + 24));
        }
        h = rotateLeft(v1, 1) + rotateLeft(v2, 7) + rotateLeft(v3, 12) + rotateLeft(v4, 18);
        h = hashMerge(h, v1);
        h = hashMerge(h, v2);
        h = hashMerge(h, v3);
        h = hashMerge(h, v4);
    } else {
        h = seed + Prime5;
    }
    h += (u64)length;
    for (; position + 8 <= length; position += 8)
        h = rotateLeft(h ^ hashRound(0, word(data + position)), 27) * Prime1 + Prime4;
    if (position + 4 <= length) {
        u32 value;
        memcpy(&value, data + position, 4);
        h = rotateLeft(h ^ ((u64)value * Prime1), 23) * Prime2 + Prime3;
        position += 4;
    }
    for (; position < length; position++)
        h = rotateLeft(h ^ ((u64)data[position] * Prime5), 11) * Prime1;
    h ^= h >> 33;
    h *= Prime2;
    h ^= h >> 29;
    h *= Prime3;
    h ^= h >> 32;
    return h;
}

int compressCapacity(int length) { return length - std::max(length / 256, MinimumSaving); }

CompressedData run(const SourcePtr& image, ByteSink& payload, int blockSize, int level, int threads, Progress& progress, const std::vector<KeptRange>* kept) {
    int blocksPerBatch = std::max(threads * 32, 64) * (0x10000 / blockSize);
    size_t capacity = (size_t)blocksPerBatch * (size_t)blockSize;
    Planner planner(image, blockSize, kept);
    std::vector<StoredPiece> pieces;
    pieces.reserve((size_t)planner.pieceCount());

    std::vector<std::unique_ptr<Batch>> batches;
    BatchQueue free, toCompress, toWrite;
    for (int i = 0; i < BatchCount; i++) {
        batches.emplace_back(new Batch(capacity, (size_t)blocksPerBatch));
        free.add(batches.back().get());
    }

    std::vector<std::unique_ptr<Lz4Encoder>> encoders;
    std::vector<std::unique_ptr<u8[]>> checks;
    for (int i = 0; i < threads; i++) {
        encoders.emplace_back(new Lz4Encoder);
        checks.emplace_back(new u8[Lz4Encoder::MaxBlockSize]);
    }

    PayloadWriter writer(payload, image->length(), blockSize);
    Failure failure;
    i64 total = image->length();
    auto shutDown = [&] {
        free.close();
        toCompress.close();
        toWrite.close();
    };

    sys::Thread reading;
    reading.start([&] {
        failure.guard([&] {
            // As many blocks as there's room for, planned as they're read.
            while (true) {
                Batch* batch = free.take();
                if (!batch)
                    return;
                if (!planner.next(batch->input.get(), (size_t)blocksPerBatch, batch->blocks, batch->virtualOffset, batch->bytes))
                    break;
                toCompress.add(batch);
            }
            image->release();
        });
        if (failure.failed())
            shutDown();
        else
            toCompress.close();
    }, sys::workerCore(0), 1 << 20, true);

    sys::Thread writing;
    writing.start([&] {
        failure.guard([&] {
            Lz4Encoder encoder;
            std::unique_ptr<u8[]> check(new u8[Lz4Encoder::MaxBlockSize]);
            while (Batch* batch = toWrite.take()) {
                for (size_t i = 0; i < batch->blocks.size(); i++) {
                    const SharedBlock& block = batch->blocks[i];
                    size_t offset = (size_t)(block.offset - batch->virtualOffset);
                    int compressed = batch->outputLength[i];
                    if (block.piece >= 0 && !block.first) {
                        if ((size_t)block.piece >= pieces.size() || !pieces[(size_t)block.piece].known)
                            throw ToolError(format("Internal error: a piece held again at RomFS offset 0x%llx wasn't stored before", (long long)block.offset));
                        const StoredPiece& piece = pieces[(size_t)block.piece];
                        if (piece.hash == batch->hashes[i]) {
                            writer.addReference(block.offset, piece, block.length, block.found);
                            continue;
                        }
                        // Not the same after all, so it's stored as anything else is.
                        compressed = compressOne(encoder, check.get(), batch->input.get() + offset, block.length, batch->output.get() + offset, level, block.offset);
                    }
                    i64 physical = compressed > 0 ? writer.addCompressed(block.offset, batch->output.get() + offset, compressed)
                                                  : writer.addStored(block.offset, batch->input.get() + offset, block.length);
                    if (block.piece >= 0 && block.first) {
                        if ((size_t)block.piece >= pieces.size())
                            pieces.resize((size_t)block.piece + 1);
                        pieces[(size_t)block.piece] = StoredPiece{physical, compressed, compressed > 0, true, batch->hashes[i]};
                    }
                }
                progress.report(batch->virtualOffset + (i64)batch->bytes, writer.position());
                free.add(batch);
            }
        });
        if (failure.failed())
            shutDown();
    }, sys::workerCore(1), 1 << 20, true);

    failure.guard([&] {
        while (Batch* batch = toCompress.take()) {
            parallelFor(threads, (int)batch->blocks.size(), [&](int index, int worker) {
                const SharedBlock& block = batch->blocks[(size_t)index];
                size_t offset = (size_t)(block.offset - batch->virtualOffset);
                // Two hashes the planner didn't use, so nothing points at a piece without all three agreeing.
                if (block.piece >= 0) {
                    const u8* bytes = batch->input.get() + offset;
                    batch->hashes[(size_t)index] = PieceHash{blockHash(bytes, (size_t)block.length, 1), blockHash(bytes, (size_t)block.length, 2)};
                }
                // Where a piece is held again, it's only compared with what was stored.
                if (block.piece >= 0 && !block.first) {
                    batch->outputLength[(size_t)index] = 0;
                    return;
                }
                batch->outputLength[(size_t)index] = compressOne(*encoders[(size_t)worker], checks[(size_t)worker].get(), batch->input.get() + offset, block.length,
                                                                 batch->output.get() + offset, level, block.offset);
            });
            toWrite.add(batch);
        }
    });

    if (failure.failed())
        shutDown();
    else
        toWrite.close();
    reading.join();
    writing.join();
    failure.raise();
    checkStop();
    return writer.finish(total);
}

Measurement measure(const SourcePtr& image, int blockSize, int level, int threads, Progress& progress, const std::vector<KeptRange>* kept, int every) {
    int blocksPerBatch = std::max(threads * 32, 64) * (0x10000 / blockSize);
    size_t capacity = (size_t)blocksPerBatch * (size_t)blockSize;
    Planner planner(image, blockSize, kept);

    // One being read while the other is gone through.
    std::vector<std::unique_ptr<Batch>> batches;
    BatchQueue free, full;
    for (int i = 0; i < 2; i++) {
        batches.emplace_back(new Batch(capacity, (size_t)blocksPerBatch));
        free.add(batches.back().get());
    }
    std::vector<std::unique_ptr<Lz4Encoder>> encoders;
    std::vector<std::unique_ptr<u8[]>> scratch;
    for (int i = 0; i < std::max(threads, 1); i++) {
        encoders.emplace_back(new Lz4Encoder);
        scratch.emplace_back(new u8[Lz4Encoder::MaxBlockSize]);
    }
    // What a block comes to compressed, with the padding before it, as the estimate counts it, or 0 if it would be stored as it is.
    auto storedSize = [&](const u8* data, int length, int worker) {
        int size = encoders[(size_t)worker]->compress(data, length, scratch[(size_t)worker].get(), compressCapacity(length), level);
        return size > 0 ? size + AveragePadding : 0;
    };

    Failure failure;
    auto shutDown = [&] {
        free.close();
        full.close();
    };
    sys::Thread reading;
    reading.start([&] {
        failure.guard([&] {
            while (true) {
                Batch* batch = free.take();
                if (!batch)
                    return;
                if (!planner.next(batch->input.get(), (size_t)blocksPerBatch, batch->blocks, batch->virtualOffset, batch->bytes))
                    break;
                full.add(batch);
            }
            image->release();
        });
        if (failure.failed())
            shutDown();
        else
            full.close();
    }, sys::workerCore(0), 1 << 20, true);

    std::vector<PieceHash> pieces;
    std::vector<char> known;
    i64 counted = 0, sampledStored = 0, sampledLength = 0, sampledLz4 = 0, sampledRaw = 0, notSampled = 0;
    i64 exactStored = 0, exactLz4 = 0, exactRaw = 0, held = 0, heldBytes = 0;
    // The successive differences between the blocks compressed to measure by, in order, for the margin.
    double previous = 0, squares = 0;
    i64 measured = 0;
    std::vector<char> compress;
    failure.guard([&] {
        while (Batch* batch = full.take()) {
            const std::vector<SharedBlock>& blocks = batch->blocks;
            compress.assign(blocks.size(), 0);
            for (size_t i = 0; i < blocks.size(); i++) {
                if (blocks[i].piece < 0 || blocks[i].first)
                    compress[i] = counted++ % every == 0 ? 1 : 0;
            }
            parallelFor(threads, (int)blocks.size(), [&](int index, int worker) {
                const SharedBlock& block = blocks[(size_t)index];
                const u8* bytes = batch->input.get() + (size_t)(block.offset - batch->virtualOffset);
                if (block.piece >= 0)
                    batch->hashes[(size_t)index] = PieceHash{blockHash(bytes, (size_t)block.length, 1), blockHash(bytes, (size_t)block.length, 2)};
                batch->outputLength[(size_t)index] = compress[(size_t)index] ? storedSize(bytes, block.length, worker) : -1;
            });

            for (size_t i = 0; i < blocks.size(); i++) {
                const SharedBlock& block = blocks[i];
                size_t piece = (size_t)block.piece;
                if (block.piece >= 0 && !block.first) {
                    if (piece < known.size() && known[piece] && pieces[piece] == batch->hashes[i]) {
                        held++;
                        heldBytes += block.length;
                        continue;
                    }
                    // Not the same after all, so it's stored as anything else is. That's rare enough to work out on its own.
                    int size = storedSize(batch->input.get() + (size_t)(block.offset - batch->virtualOffset), block.length, 0);
                    exactStored += size > 0 ? size : block.length;
                    if (size > 0)
                        exactLz4++;
                    else
                        exactRaw++;
                    continue;
                }
                if (block.piece >= 0) {
                    if (piece >= known.size()) {
                        known.resize(piece + 1, 0);
                        pieces.resize(piece + 1);
                    }
                    pieces[piece] = batch->hashes[i];
                    known[piece] = 1;
                }
                int compressed = batch->outputLength[i];
                if (compressed < 0) {
                    notSampled += block.length;
                    continue;
                }
                i64 stored = compressed > 0 ? compressed : block.length;
                sampledStored += stored;
                sampledLength += block.length;
                if (compressed > 0)
                    sampledLz4++;
                else
                    sampledRaw++;
                double ratio = (double)stored / (double)block.length;
                if (measured > 0) {
                    // Kept from being fused into one multiply-add, which the PC doesn't do, so the sum comes out the same on both.
                    volatile double square = (ratio - previous) * (ratio - previous);
                    squares += square;
                }
                previous = ratio;
                measured++;
            }
            progress.report(batch->virtualOffset + (i64)batch->bytes, 0);
            free.add(batch);
        }
    });
    if (failure.failed())
        shutDown();
    reading.join();
    failure.raise();
    checkStop();

    // What's stored where it is goes by the blocks compressed to measure by. The margin is for a sample taken evenly across, with the share
    // of it that was compressed taken off.
    Measurement result;
    i64 total = sampledLength + notSampled;
    double scale = sampledLength > 0 ? (double)total / (double)sampledLength : 0;
    double share = total > 0 ? (double)sampledLength / (double)total : 1;
    double variance = measured > 1 ? (1 - share) / (double)measured * squares / (2.0 * (double)(measured - 1)) : 0;
    result.stored = exactStored + (i64)((double)sampledStored * scale);
    result.margin = 1.96 * sqrt(variance) * (double)total;
    result.lz4Blocks = exactLz4 + (i64)((double)sampledLz4 * scale);
    result.rawBlocks = exactRaw + (i64)((double)sampledRaw * scale);
    result.held = held;
    result.heldBytes = heldBytes;
    return result;
}

}  // namespace RomFsCompressor

// -------------------------------------------------------------------------------------------------------------------
// Sections

namespace Sections {

namespace {

constexpr u8 EncryptionNone = 1;
constexpr u8 EncryptionAesCtr = 3;
constexpr int Pfs0Alignment = 0x200;

/// hacBrewPack always pads the table, by a whole 0x200 when it is already aligned.
i64 pfs0DataOffset(i64 tableSize) { return tableSize + (Pfs0Alignment - tableSize % Pfs0Alignment); }

Bytes newFsHeader(u8 fsType, u8 hashType, bool encrypted) {
    Bytes header(0x200);
    header[0] = 2;
    header[2] = fsType;
    header[3] = hashType;
    header[4] = encrypted ? EncryptionAesCtr : EncryptionNone;
    return header;
}

Bytes integrityHeader(const IvfcResult& ivfc, bool encrypted, const CompressedData* compressed) {
    Bytes header = newFsHeader(0, 3, encrypted);
    u8* h = header.data();
    memcpy(h + 0x08, "IVFC", 4);
    wr32(h + 0x0C, 0x20000);
    wr32(h + 0x10, 0x20);
    wr32(h + 0x14, 7);
    for (int i = 0; i < 6; i++) {
        u8* level = h + 0x18 + i * 0x18;
        wr64(level, (u64)ivfc.levelOffsets[i]);
        wr64(level + 8, (u64)ivfc.levelSizes[i]);
        wr32(level + 0x10, IvfcBuilder::BlockOrder);
    }
    memcpy(h + 0xC8, ivfc.masterHash, 32);

    if (compressed) {
        u8* info = h + 0x178;
        wr64(info, (u64)compressed->tableOffset);
        wr64(info + 8, (u64)compressed->tableSize);
        BucketTree::writeHeader(info + 0x10, compressed->entryCount);
    }
    return header;
}

}  // namespace

i64 pfs0SectionSize(i64 pfs0Size, int hashBlockSize) {
    i64 table = (pfs0Size + hashBlockSize - 1) / hashBlockSize * 0x20;
    return pfs0DataOffset(table) + pfs0Size;
}

i64 romFsSectionSize(i64 imageSize) {
    i64 data = imageSize + IvfcBuilder::padSize(imageSize);
    return IvfcBuilder::hashLevelsSize(data) + data;
}

i64 compressedRomFsSectionSize(i64 storedSize, int tableEntries) {
    i64 aligned = (storedSize + IvfcBuilder::BlockSize - 1) & ~(IvfcBuilder::BlockSize - 1LL);
    i64 data = aligned + BucketTree::nodeStorageSize(tableEntries) + BucketTree::entryStorageSize(tableEntries);
    return IvfcBuilder::hashLevelsSize(data) + data;
}

NcaSection pfs0(const SourcePtr& source, int hashBlockSize, bool encrypted) {
    i64 length = source->length();
    i64 blocks = (length + hashBlockSize - 1) / hashBlockSize;
    Bytes table((size_t)blocks * 0x20);
    Bytes buffer((size_t)hashBlockSize);
    for (i64 i = 0; i < blocks; i++) {
        size_t n = (size_t)std::min<i64>(hashBlockSize, length - i * hashBlockSize);
        source->read(i * hashBlockSize, buffer.data(), n);
        sha256(buffer.data(), n, table.data() + i * 0x20);
    }
    source->release();

    i64 pfs0Offset = pfs0DataOffset((i64)table.size());

    Bytes header = newFsHeader(1, 2, encrypted);
    u8* h = header.data();
    sha256(table.data(), table.size(), h + 0x08);
    wr32(h + 0x28, (u32)hashBlockSize);
    wr32(h + 0x2C, 2);
    wr64(h + 0x38, (u64)table.size());
    wr64(h + 0x40, (u64)pfs0Offset);
    wr64(h + 0x48, (u64)length);

    i64 tableSize = (i64)table.size();
    auto data = std::make_shared<ConcatSource>();
    data->add(bytesSource(std::move(table))).add(std::make_shared<ZeroSource>(pfs0Offset - tableSize)).add(source);

    NcaSection section;
    section.fsHeader = std::move(header);
    section.data = data;
    section.encrypted = encrypted;
    return section;
}

NcaSection romFs(const SourcePtr& image, bool encrypted, Progress* progress) {
    IvfcBuilder ivfc;
    Bytes buffer((size_t)IvfcBuilder::BlockSize * 512);
    i64 padding = IvfcBuilder::padSize(image->length());
    i64 total = image->length() + padding;
    ivfc.expect(total);
    for (i64 position = 0; position < total;) {
        size_t n = (size_t)std::min<i64>(total - position, (i64)buffer.size());
        size_t fromImage = (size_t)std::min<i64>((i64)n, std::max<i64>(image->length() - position, 0));
        image->read(position, buffer.data(), fromImage);
        memset(buffer.data() + fromImage, 0, n - fromImage);
        ivfc.add(buffer.data(), n);
        position += (i64)n;
        if (progress)
            progress->report(std::min(position, image->length()), position);
    }
    image->release();

    IvfcResult result = ivfc.finish(image->length());
    Bytes header = integrityHeader(result, encrypted, nullptr);
    auto data = std::make_shared<ConcatSource>();
    data->add(bytesSource(result.hashLevels)).add(image).add(std::make_shared<ZeroSource>(padding));

    NcaSection section;
    section.fsHeader = std::move(header);
    section.data = data;
    section.encrypted = encrypted;
    return section;
}

Bytes compressedRomFsHeader(const CompressedData& compressed, bool encrypted) { return integrityHeader(compressed.ivfc, encrypted, &compressed); }

}  // namespace Sections

SourcePtr buildPfs0(const std::vector<std::pair<std::string, SourcePtr>>& files) {
    std::string strings;
    std::vector<int> offsets;
    for (auto& file : files) {
        offsets.push_back((int)strings.size());
        strings += file.first;
        strings += '\0';
    }
    int tableSize = (int)((strings.size() + 0x1F) & ~(size_t)0x1F);

    Bytes header(0x10 + files.size() * 0x18 + (size_t)tableSize);
    u8* h = header.data();
    memcpy(h, "PFS0", 4);
    wr32(h + 4, (u32)files.size());
    wr32(h + 8, (u32)tableSize);
    i64 offset = 0;
    for (size_t i = 0; i < files.size(); i++) {
        u8* e = h + 0x10 + i * 0x18;
        wr64(e, (u64)offset);
        wr64(e + 8, (u64)files[i].second->length());
        wr32(e + 0x10, (u32)offsets[i]);
        offset += files[i].second->length();
    }
    if (!strings.empty())
        memcpy(h + 0x10 + files.size() * 0x18, strings.data(), strings.size());

    auto result = std::make_shared<ConcatSource>();
    result->add(bytesSource(std::move(header)));
    for (auto& file : files)
        result->add(file.second);
    return result;
}

// -------------------------------------------------------------------------------------------------------------------
// Writing

NcaWriter::NcaWriter(const u8* headerKey, const u8* keyAreaEncryptionKey, const u8* sectionKey, int keyGeneration)
    : _headerKey(headerKey, headerKey + 32), _keyAreaEncryptionKey(keyAreaEncryptionKey, keyAreaEncryptionKey + 16), _sectionKey(sectionKey, sectionKey + 16), _keyGeneration(keyGeneration) {}

i64 NcaWriter::totalSize(const NcaSections& sections) {
    i64 position = HeaderRegionSize;
    for (auto& section : sections) {
        if (section)
            position += alignToSector(section->data->length());
    }
    return position;
}

Bytes NcaWriter::header(NcaContentType contentType, u64 titleId, const std::vector<Place>& places, i64 total, bool sign, int contentIndex, u32 sdkOverride) const {
    Bytes header(HeaderRegionSize);
    u8* h = header.data();
    memcpy(h + 0x200, "NCA3", 4);
    h[0x205] = (u8)contentType;
    if (_keyGeneration >= 2)
        h[0x206] = 2;
    if (_keyGeneration >= 3)
        h[0x220] = (u8)_keyGeneration;
    wr64(h + 0x210, titleId);
    wr32(h + 0x218, (u32)contentIndex);
    wr32(h + 0x21C, sdkOverride);

    for (size_t i = 0; i < places.size(); i++) {
        if (!places[i].used)
            continue;
        u8* entry = h + 0x240 + i * 0x10;
        wr32(entry, (u32)(places[i].start / 0x200));
        wr32(entry + 4, (u32)(places[i].end / 0x200));
        wr32(entry + 8, 1);
        sha256(places[i].fsHeader.data(), places[i].fsHeader.size(), h + 0x280 + i * 0x20);
        memcpy(h + 0x400 + i * 0x200, places[i].fsHeader.data(), 0x200);
    }
    wr64(h + 0x208, (u64)total);

    u8 keyArea[0x40] = {};
    memcpy(keyArea + 0x20, _sectionKey.data(), 16);
    aesEcbEncrypt(_keyAreaEncryptionKey.data(), keyArea, sizeof(keyArea));
    memcpy(h + 0x300, keyArea, sizeof(keyArea));

    if (sign)
        signPss(h + 0x200, 0x200, h + 0x100);

    aesXts(_headerKey.data(), h, HeaderRegionSize, true);
    return header;
}

NcaInfo NcaWriter::write(NcaStream& output, NcaContentType contentType, u64 titleId, NcaSections& sections, bool sign, Progress* progress, int contentIndex, u32 sdkOverride) {
    constexpr size_t ChunkSize = 1 << 20;

    i64 position = HeaderRegionSize;
    std::vector<i64> sizes(sections.size());
    std::vector<Place> places(sections.size());
    for (size_t i = 0; i < sections.size(); i++) {
        if (!sections[i])
            continue;
        sizes[i] = alignToSector(sections[i]->data->length());
        places[i].used = true;
        places[i].start = position;
        places[i].end = position + sizes[i];
        places[i].fsHeader = sections[i]->fsHeader;
        position += sizes[i];
    }
    Bytes head = header(contentType, titleId, places, position, sign, contentIndex, sdkOverride);

    Sha256 hash;
    output.write(head.data(), HeaderRegionSize);
    hash.update(head.data(), HeaderRegionSize);

    std::unique_ptr<u8[]> buffer(new u8[ChunkSize]);
    i64 fileOffset = HeaderRegionSize;
    i64 written = 0;
    AesCtr ctr(_sectionKey.data());
    for (size_t i = 0; i < sections.size(); i++) {
        if (!sections[i])
            continue;
        NcaSection& section = *sections[i];
        i64 dataLength = section.data->length();
        for (i64 done = 0; done < sizes[i];) {
            checkStop();
            size_t n = (size_t)std::min<i64>((i64)ChunkSize, sizes[i] - done);
            size_t fromData = (size_t)std::min<i64>((i64)n, std::max<i64>(dataLength - done, 0));
            section.data->read(done, buffer.get(), fromData);
            memset(buffer.get() + fromData, 0, n - fromData);
            if (section.encrypted)
                ctr.crypt(buffer.get(), n, fileOffset, section.upperIv);
            hash.update(buffer.get(), n);
            output.write(buffer.get(), n);
            done += (i64)n;
            fileOffset += (i64)n;
            written += (i64)n;
            if (progress)
                progress->report(written, written);
        }
        section.data->release();
    }

    NcaInfo info;
    hash.finish(info.hash);
    info.size = position;
    return info;
}

// -------------------------------------------------------------------------------------------------------------------
// Writing while it's made

NcaBuilder::NcaBuilder(NcaWriter& writer, NcaStream& output, i64 holdInMemory, bool compareReadBack)
    : _writer(writer), _output(output), _holdLeft(holdInMemory), _compare(compareReadBack), _ctr(writer._sectionKey.data()), _chunk(new u8[ChunkSize]), _position(NcaWriter::HeaderRegionSize), _places(NcaFile::SectionCount) {
    Bytes reserved(NcaWriter::HeaderRegionSize);
    _output.write(reserved.data(), reserved.size());
}

void NcaBuilder::write(const u8* data, size_t size) {
    while (size > 0) {
        size_t n = std::min(size, ChunkSize - _chunkUsed);
        memcpy(_chunk.get() + _chunkUsed, data, n);
        _chunkUsed += n;
        data += n;
        size -= n;
        if (_chunkUsed == ChunkSize)
            flush();
    }
}

void NcaBuilder::flush() {
    if (_chunkUsed == 0)
        return;
    if (_encrypted)
        _ctr.crypt(_chunk.get(), _chunkUsed, _position, _upperIv);

    // Whatever isn't kept gets read back later on, and has to come back the same.
    bool holding = _holdLeft >= (i64)_chunkUsed && _held.size() == _written.size();
    if (holding) {
        std::unique_ptr<u8[]> copy(new (std::nothrow) u8[_chunkUsed]);
        if (copy) {
            memcpy(copy.get(), _chunk.get(), _chunkUsed);
            _held.push_back(std::move(copy));
            _holdLeft -= (i64)_chunkUsed;
        } else {
            holding = false;
        }
    }
    Written written;
    written.size = (u32)_chunkUsed;
    if (!holding && _compare)
        sha256(_chunk.get(), _chunkUsed, written.digest);
    _written.push_back(written);

    _output.write(_chunk.get(), _chunkUsed);
    _position += (i64)_chunkUsed;
    _chunkUsed = 0;
}

void NcaBuilder::startSection(bool encrypted, u64 upperIv) {
    _sectionStart = _position;
    _encrypted = encrypted;
    _upperIv = upperIv;
}

void NcaBuilder::endSection(int index, Bytes fsHeader) {
    static const u8 zeros[0x200] = {};
    i64 length = _position + (i64)_chunkUsed - _sectionStart;
    write(zeros, (size_t)(NcaWriter::alignToSector(length) - length));
    flush();

    NcaWriter::Place& place = _places.at((size_t)index);
    place.used = true;
    place.start = _sectionStart;
    place.end = _position;
    place.fsHeader = std::move(fsHeader);
}

void NcaBuilder::add(int index, NcaSection section) {
    constexpr size_t PieceSize = 1 << 20;
    startSection(section.encrypted, section.upperIv);
    Bytes buffer(PieceSize);
    i64 length = section.data->length();
    for (i64 done = 0; done < length;) {
        checkStop();
        size_t n = (size_t)std::min<i64>((i64)PieceSize, length - done);
        section.data->read(done, buffer.data(), n);
        write(buffer.data(), n);
        done += (i64)n;
    }
    section.data->release();
    endSection(index, std::move(section.fsHeader));
}

void NcaBuilder::addCompressed(int index, const SourcePtr& image, int blockSize, int level, int threads, bool encrypted, Progress& progress, CompressedData* details,
                               const std::vector<KeptRange>* kept) {
    startSection(encrypted, 0);
    CompressedData data = RomFsCompressor::run(image, *this, blockSize, level, threads, progress, kept);
    write(data.ivfc.hashLevels.data(), data.ivfc.hashLevels.size());
    endSection(index, Sections::compressedRomFsHeader(data, encrypted));
    if (details) {
        data.ivfc.hashLevels = Bytes();
        *details = std::move(data);
    }
}

NcaInfo NcaBuilder::finish(NcaContentType contentType, u64 titleId, bool sign, int contentIndex, u32 sdkOverride) {
    Bytes head = _writer.header(contentType, titleId, _places, _position, sign, contentIndex, sdkOverride);
    _output.overwrite(0, head.data(), head.size());

    Sha256 hash;
    hash.update(head.data(), head.size());
    i64 position = NcaWriter::HeaderRegionSize;
    size_t index = 0;
    if (!_held.empty()) {
        i64 inMemory = 0;
        for (size_t i = 0; i < _held.size(); i++)
            inMemory += _written[i].size;
        i64 begun = position;
        Progress fromMemory("正在计算内存中剩余内容的哈希", inMemory);
        for (; index < _held.size(); index++) {
            hash.update(_held[index].get(), _written[index].size);
            position += _written[index].size;
            _held[index].reset();
            fromMemory.report(position - begun);
        }
        fromMemory.done();
    }

    if (position < _position) {
        Progress progress(_compare ? _output.readBackLabel() : "Getting it ready for the console", _position - position);
        i64 start = position;

        if (_compare) {
            Bytes back(head.size());
            _output.readBack(0, back.data(), back.size());
            if (back != head)
                throw ToolError("写入内容回读不一致，存储卡可能有问题");
        }

        // The next piece is already on its way while this one gets hashed, so neither the card nor the processor is waiting for the other.
        constexpr int PiecesAhead = 3;
        struct Piece {
            std::unique_ptr<u8[]> bytes;
            size_t index = 0;
        };
        std::vector<std::unique_ptr<Piece>> pieces;
        RomFsCompressor::Handover<Piece> empty, full;
        for (int i = 0; i < PiecesAhead; i++) {
            pieces.emplace_back(new Piece);
            pieces.back()->bytes.reset(new u8[ChunkSize]);
            empty.add(pieces.back().get());
        }

        Failure failure;
        sys::Thread reading;
        size_t first = index;
        i64 firstPosition = position;
        reading.start([&] {
            failure.guard([&] {
                i64 at = firstPosition;
                for (size_t i = first; i < _written.size(); i++) {
                    Piece* piece = empty.take();
                    if (!piece)
                        return;
                    _output.readBack(at, piece->bytes.get(), _written[i].size);
                    piece->index = i;
                    at += _written[i].size;
                    full.add(piece);
                }
            });
            full.close();
        }, sys::workerCore(1), 1 << 20, true);

        failure.guard([&] {
            while (Piece* piece = full.take()) {
                checkStop();
                const Written& written = _written[piece->index];
                if (_compare) {
                    u8 digest[DigestSize];
                    sha256(piece->bytes.get(), written.size, digest);
                    if (memcmp(digest, written.digest, DigestSize) != 0)
                        throw ToolError("写入内容回读不一致，存储卡可能有问题");
                }
                hash.update(piece->bytes.get(), written.size);
                position += written.size;
                progress.report(position - start);
                empty.add(piece);
            }
        });
        empty.close();
        full.close();
        reading.join();
        failure.raise();
        progress.done();
    }

    NcaInfo info;
    hash.finish(info.hash);
    info.size = _position;
    return info;
}

}  // namespace hbp
