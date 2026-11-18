#include "ncz.h"

#include <algorithm>
#include <new>

namespace hbp {

namespace {

constexpr int HeaderSize = 0x4000;
constexpr int SectionEntrySize = 0x40;
constexpr u64 MaxSections = 0x100000;
constexpr int MinBlockExponent = 14;
constexpr int MaxBlockExponent = 30;
constexpr u8 BlockTypeZstd = 1;
constexpr i64 MaxKeptSection = 256LL << 20;
/// An update's stream goes back about three times by itself, to read the tables at its end before its data, and then reads straight on: kept
/// from the third time, as it used to be, it was written in full and never read. Going back more than that, it keeps on doing so.
constexpr int RewindsBeforeSpooling = 3;
constexpr int NcaHeaderRegionSize = 0xC00;

std::atomic<int> g_lastSweep{0};
thread_local int t_sweep = 0;
std::string g_spoolFolder;
std::atomic<int> g_spoolNumber{0};
/// What's left of the memory solid streams can be kept in, between them.
std::atomic<i64> g_spoolMemory{0};
constexpr i64 MemoryPiece = 64LL << 20;

/// Tests go back fewer times, with games small enough that only their updates do.
int rewindsBeforeSpooling() {
    const char* test = getenv("HBP_TEST_REWINDS_BEFORE_KEEPING");
    return test && *test ? atoi(test) : RewindsBeforeSpooling;
}

}  // namespace

void NczSource::beginSinglePass() { t_sweep = ++g_lastSweep; }
void NczSource::endSinglePass() { t_sweep = 0; }
void NczSource::setSpoolFolder(const std::string& folder) { g_spoolFolder = folder; }
void NczSource::setSpoolMemory(i64 bytes) { g_spoolMemory = std::max<i64>(bytes, 0); }

void NczSource::fail() const { throw ToolError(_name + " 已损坏，或不是 NCZ"); }

NczSource::NczSource(SourcePtr file, std::string name) : _file(std::move(file)), _name(std::move(name)) {
    if (_file->length() < HeaderSize + 16)
        fail();

    _header.resize(HeaderSize);
    _file->read(0, _header.data(), HeaderSize);

    u8 head[16];
    _file->read(HeaderSize, head, sizeof(head));
    // NXPress's NCZSECT2 is the same, for a game cut down to go with its update: each piece stored is a section of its own, decrypted as the
    // rest, with how far its counter is from where it is. The pieces are read as they're stored, so only that it's sound matters here.
    // NCZSECT3 is NXPress's archive kind, which only it can unpack. As NczSource on the PC.
    bool withCounterOffsets = memcmp(head, "NCZSECT2", 8) == 0;
    if (memcmp(head, "NCZSECT3", 8) == 0)
        throw ToolError(_name + " is an archive only NXPress reads: unpack it with NXPress first");
    if (!withCounterOffsets && memcmp(head, "NCZSECTN", 8) != 0)
        fail();
    u64 count = rd64(head + 8);
    if (count == 0 || count > MaxSections || HeaderSize + 16 + (i64)count * SectionEntrySize > _file->length())
        fail();

    Bytes table((size_t)count * SectionEntrySize);
    _file->read(HeaderSize + 16, table.data(), table.size());
    i64 position = HeaderSize + 16 + (i64)table.size();

    // The stream holds the sections one after another, starting with any gap before the first. Normally the header is everything
    // before the first section. An NCA laid out like hacBrewPack's has a section start inside the part that's kept as it is, and
    // then the decrypted copy in the stream is what counts.
    for (size_t i = 0; i < (size_t)count; i++) {
        const u8* e = table.data() + i * SectionEntrySize;
        i64 start = rdi64(e);
        i64 size = rdi64(e + 8);
        if (start < NcaHeaderRegionSize || size < 0 || start > INT64_MAX - size)
            fail();
        // A counter offset is whole AES blocks, and only on what's encrypted with a counter.
        i64 counterOffset = withCounterOffsets ? rdi64(e + 0x18) : 0;
        u64 crypto = rd64(e + 0x10);
        if (counterOffset % 16 != 0 || (counterOffset != 0 && crypto != 3 && crypto != 4))
            fail();
        if (i == 0 && start > HeaderSize) {
            _ncaStarts.push_back(HeaderSize);
            _sizes.push_back(start - HeaderSize);
        }
        _ncaStarts.push_back(start);
        _sizes.push_back(size);
    }

    _headerEnd = (int)std::min<i64>(_ncaStarts[0], HeaderSize);
    _streamStarts.resize(_ncaStarts.size());
    i64 streamSize = 0;
    i64 end = HeaderSize;
    for (size_t i = 0; i < _ncaStarts.size(); i++) {
        if (i > 0 && _ncaStarts[i] < _ncaStarts[i - 1] + _sizes[i - 1])
            fail();
        _streamStarts[i] = streamSize;
        if (_sizes[i] > INT64_MAX - streamSize)
            fail();
        streamSize += _sizes[i];
        end = _ncaStarts[i] + _sizes[i];
    }
    _streamSize = streamSize;
    _length = end;

    u8 blockMagic[8] = {};
    if (position + 8 <= _file->length())
        _file->read(position, blockMagic, 8);
    if (memcmp(blockMagic, "NCZBLOCK", 8) == 0) {
        u8 blockHead[8 + 16];
        if (position + (i64)sizeof(blockHead) > _file->length())
            fail();
        _file->read(position, blockHead, sizeof(blockHead));
        if (blockHead[8 + 1] != BlockTypeZstd)
            throw ToolError(format("%s uses compression type %d, which isn't supported", _name.c_str(), blockHead[8 + 1]));
        _blockExponent = blockHead[8 + 3];
        int blocks = rdi32(blockHead + 8 + 4);
        i64 decompressed = rdi64(blockHead + 8 + 8);
        position += (i64)sizeof(blockHead);

        // The blocks may hold a little more than the sections do, if the NCA had anything after its last section.
        i64 expected = _blockExponent >= MinBlockExponent && _blockExponent <= MaxBlockExponent && decompressed >= streamSize
                           ? (decompressed + (1LL << _blockExponent) - 1) >> _blockExponent
                           : -1;
        if (blocks != expected || blocks <= 0 || position + blocks * 4LL > _file->length())
            fail();
        _streamSize = decompressed;

        Bytes list((size_t)blocks * 4);
        _file->read(position, list.data(), list.size());
        position += (i64)list.size();

        _blockOffsets.resize((size_t)blocks);
        _blockSizes.resize((size_t)blocks);
        i64 offset = position;
        for (int i = 0; i < blocks; i++) {
            int size = rdi32(list.data() + i * 4);
            if (size <= 0 || size > 1LL << _blockExponent || offset + size > _file->length())
                fail();
            _blockOffsets[(size_t)i] = offset;
            _blockSizes[(size_t)i] = size;
            offset += size;
        }
        _blockDecoder.reset(new ZstdDecoder);
    }
    _dataStart = position;
}

NczSource::~NczSource() { releaseMemory(); }

void NczSource::keepIfSmall(i64 ncaOffset, i64 size) {
    // Tests make it smaller, as their games are, for a solid stream to be read out of order.
    const char* test = getenv("HBP_TEST_MOST_KEPT_SECTION");
    i64 most = test && *test ? atoll(test) : MaxKeptSection;
    if (!isSolid() || size <= 0 || size > most)
        return;
    int segment = findSegment(ncaOffset);
    if (segment < 0 || _ncaStarts[(size_t)segment] > ncaOffset)
        return;
    i64 start = _streamStarts[(size_t)segment] + (ncaOffset - _ncaStarts[(size_t)segment]);

    // The whole section has to sit in the stream in one piece.
    int last = findSegment(ncaOffset + size - 1);
    if (last < 0 || _streamStarts[(size_t)last] + (ncaOffset + size - 1 - _ncaStarts[(size_t)last]) != start + size - 1)
        return;
    auto range = std::make_unique<KeptRange>();
    range->start = start;
    range->size = size;
    _kept.push_back(std::move(range));
}

void NczSource::releaseMemory() {
    _stream.reset();
    if (_spool) {
        _spool.reset();
        sys::removeFile(_spoolPath);
    }
    _memorySpool.clear();
    g_spoolMemory += _memoryTaken;
    _memoryTaken = 0;
    _spooled = 0;
    _rewinds = 0;
    _block.reset();
    _compressedBlock.reset();
    _blockIndex = -1;
    for (auto& range : _kept) {
        range->data.reset();
        range->filled = 0;
    }
}

void NczSource::read(i64 offset, void* buffer, size_t count) {
    if (offset < 0 || offset + (i64)count > _length)
        throw ToolError("内部错误：读取超出 NCZ 范围");

    u8* out = (u8*)buffer;
    while (count > 0) {
        size_t n;
        if (offset < _headerEnd) {
            n = std::min(count, (size_t)(_headerEnd - offset));
            memcpy(out, _header.data() + offset, n);
        } else {
            int segment = findSegment(offset);
            i64 inSegment = offset - _ncaStarts[(size_t)segment];
            if (inSegment >= _sizes[(size_t)segment]) {
                // Nothing is stored for a gap between sections, and nothing belongs in one either.
                i64 gapEnd = (size_t)segment + 1 < _ncaStarts.size() ? _ncaStarts[(size_t)segment + 1] : _length;
                n = (size_t)std::min<i64>((i64)count, gapEnd - offset);
                memset(out, 0, n);
            } else {
                n = (size_t)std::min<i64>((i64)count, _sizes[(size_t)segment] - inSegment);
                i64 position = _streamStarts[(size_t)segment] + inSegment;
                if (isSolid())
                    readSolid(position, out, n);
                else
                    readBlocks(position, out, n);
            }
        }
        offset += (i64)n;
        out += n;
        count -= n;
    }
}

/// The last segment starting at or before an NCA offset.
int NczSource::findSegment(i64 offset) const {
    auto found = std::upper_bound(_ncaStarts.begin(), _ncaStarts.end(), offset);
    int index = (int)(found - _ncaStarts.begin()) - 1;
    return std::max(index, 0);
}

void NczSource::readBlocks(i64 position, u8* buffer, size_t count) {
    while (count > 0) {
        int index = (int)(position >> _blockExponent);
        loadBlock(index);
        int inBlock = (int)(position - ((i64)index << _blockExponent));
        size_t n = std::min(count, (size_t)(blockLength(index) - inBlock));
        memcpy(buffer, _block.get() + inBlock, n);
        position += (i64)n;
        buffer += n;
        count -= n;
    }
}

int NczSource::blockLength(int index) const {
    return (int)std::min(1LL << _blockExponent, (long long)(_streamSize - ((i64)index << _blockExponent)));
}

void NczSource::loadBlock(int index) {
    if (_blockIndex == index)
        return;
    checkStop();

    size_t blockSize = (size_t)1 << _blockExponent;
    if (!_block) {
        _block.reset(new (std::nothrow) u8[blockSize]);
        if (!_block)
            throw ToolError(format("There isn't enough free memory to read %s: it needs %s", _name.c_str(), formatSize((i64)blockSize).c_str()));
    }
    _blockIndex = -1;
    int length = blockLength(index);
    int size = _blockSizes[(size_t)index];

    // A block that didn't shrink is stored as it is.
    if (size >= length) {
        if (size != length)
            fail();
        _file->read(_blockOffsets[(size_t)index], _block.get(), (size_t)length);
    } else {
        if (!_compressedBlock) {
            _compressedBlock.reset(new (std::nothrow) u8[blockSize]);
            if (!_compressedBlock)
                throw ToolError(format("There isn't enough free memory to read %s: it needs %s", _name.c_str(), formatSize((i64)blockSize).c_str()));
        }
        _file->read(_blockOffsets[(size_t)index], _compressedBlock.get(), (size_t)size);
        if (_blockDecoder->decompress(_compressedBlock.get(), (size_t)size, _block.get(), (size_t)length) != (size_t)length)
            fail();
    }
    _blockIndex = index;
}

void NczSource::readSolid(i64 position, u8* buffer, size_t count) {
    bool mayRewind = t_sweep == 0 || _sweepSeen != t_sweep;
    while (count > 0) {
        size_t n = readKept(position, buffer, count);
        if (n == 0 && position < _spooled) {
            n = (size_t)std::min<i64>((i64)count, _spooled - position);
            readCopy(position, buffer, n);
        }
        if (n == 0) {
            if (!_stream) {
                _stream.reset(new ZstdStream(_file, _dataStart, _file->length() - _dataStart, _streamSize));
                _stream->decoded = [this](i64 p, const u8* d, size_t c) { onDecoded(p, d, c); };
            }
            ZstdStream& stream = *_stream;
            if (position < stream.oldest()) {
                if (!mayRewind)
                    throw NczRewind();
                rewind(stream);
            }
            while (position >= stream.position()) {
                if (!stream.advance())
                    fail();
            }
            n = stream.copy(position, buffer, count);
        }
        position += (i64)n;
        buffer += n;
        count -= n;
    }
    _sweepSeen = t_sweep;
}

size_t NczSource::readKept(i64 position, u8* buffer, size_t count) {
    for (auto& range : _kept) {
        i64 inRange = position - range->start;
        if (inRange >= 0 && inRange < range->filled) {
            size_t n = (size_t)std::min<i64>((i64)count, range->filled - inRange);
            memcpy(buffer, range->data.get() + inRange, n);
            return n;
        }
    }
    return 0;
}

void NczSource::rewind(ZstdStream& stream) {
    if (t_sweep == 0 && !keeping()) {
        // In memory, if all of it fits, from the first time: that costs nothing but memory, where decoding it again costs a pass over it.
        if (takeMemory(true) > 0) {
            _spooled = 0;
            sink().message(format("%s is needed out of order, so what's decoded of it is kept in memory, up to %s", _name.c_str(), formatSize(_streamSize).c_str()));
        } else if (++_rewinds > rewindsBeforeSpooling()) {
            keepInMemoryAndFile();
        }
    }

    if (!_warned) {
        _warned = true;
        sink().message(_name + " 使用固态压缩，必须从头重新解码，耗时较长");
    }
    stream.rewind();
}

void NczSource::onDecoded(i64 position, const u8* data, size_t count) {
    for (auto& range : _kept) {
        i64 next = range->start + range->filled;
        if (range->filled >= range->size || next < position || next >= position + (i64)count)
            continue;
        if (!range->data) {
            range->data.reset(new (std::nothrow) u8[(size_t)range->size]);
            if (!range->data)
                throw ToolError(format("There isn't enough free memory to read %s", _name.c_str()));
        }
        size_t n = (size_t)std::min<i64>(position + (i64)count - next, range->size - range->filled);
        memcpy(range->data.get() + range->filled, data + (next - position), n);
        range->filled += (i64)n;
    }

    if (position == _spooled && keeping())
        _spooled += (i64)writeCopy(position, data, count);
}

/// Memory for the stream from its start, from what's left for all of them, so that two kept at once, such as a game's and its update's, can't
/// take more than there is between them: all of it or none, or else as much as there is. It's all had at once, so that running out shows up
/// now rather than part of the way through. How much it got.
i64 NczSource::takeMemory(bool allOrNone) {
    i64 left = g_spoolMemory.load(), taken;
    do {
        taken = std::min(_streamSize, left);
        if (taken <= 0 || (allOrNone && taken < _streamSize))
            return 0;
    } while (!g_spoolMemory.compare_exchange_weak(left, left - taken));
    for (i64 at = 0; at < taken; at += MemoryPiece) {
        _memorySpool.emplace_back(new (std::nothrow) u8[(size_t)std::min(MemoryPiece, taken - at)]);
        if (!_memorySpool.back()) {
            _memorySpool.clear();
            g_spoolMemory += taken;
            return 0;
        }
    }
    _memoryTaken = taken;
    return taken;
}

/// For a stream that keeps going back: what there's memory for in memory, and the rest in a temporary file.
void NczSource::keepInMemoryAndFile() {
    i64 inMemory = takeMemory(false);
    std::string folder = g_spoolFolder.empty() ? std::string(".") : g_spoolFolder;
    if (inMemory < _streamSize) {
        try {
            sys::makeDirectories(folder);
            _spoolPath = format("%s/.hacbrewpacklz_%d.tmp", folder.c_str(), ++g_spoolNumber);
            _spool.reset(new sys::File(_spoolPath, sys::File::Mode::Create, _streamSize - inMemory));
        } catch (...) {
            _memorySpool.clear();
            g_spoolMemory += _memoryTaken;
            _memoryTaken = 0;
            throw;
        }
    }
    _spooled = 0;
    if (inMemory == 0)
        sink().message(format("%s keeps being needed out of order, so a temporary copy of up to %s is being kept in %s", _name.c_str(), formatSize(_streamSize).c_str(), folder.c_str()));
    else if (inMemory < _streamSize)
        sink().message(format("%s keeps being needed out of order, so what's decoded of it is kept: the first %s in memory, and a temporary copy of the rest, up to %s, in %s",
                              _name.c_str(), formatSize(inMemory).c_str(), formatSize(_streamSize - inMemory).c_str(), folder.c_str()));
    else
        sink().message(format("%s keeps being needed out of order, so what's decoded of it is kept in memory, up to %s", _name.c_str(), formatSize(_streamSize).c_str()));
}

void NczSource::readCopy(i64 position, u8* buffer, size_t count) {
    while (count > 0) {
        size_t n = count;
        if (position < _memoryTaken) {
            i64 inPiece = position % MemoryPiece;
            n = (size_t)std::min<i64>((i64)n, std::min(MemoryPiece - inPiece, _memoryTaken - position));
            memcpy(buffer, _memorySpool[(size_t)(position / MemoryPiece)].get() + inPiece, n);
        } else {
            _spool->read(position - _memoryTaken, buffer, n);
        }
        position += (i64)n;
        buffer += n;
        count -= n;
    }
}

/// What's decoded next, kept: in memory as far as that goes, and then in the file, if there is one. How much of it was kept.
size_t NczSource::writeCopy(i64 position, const u8* data, size_t count) {
    count = (size_t)std::min<i64>((i64)count, _streamSize - position);
    size_t kept = 0;
    while (kept < count) {
        size_t n = count - kept;
        if (position < _memoryTaken) {
            i64 inPiece = position % MemoryPiece;
            n = (size_t)std::min<i64>((i64)n, std::min(MemoryPiece - inPiece, _memoryTaken - position));
            memcpy(_memorySpool[(size_t)(position / MemoryPiece)].get() + inPiece, data, n);
        } else if (_spool) {
            _spool->write(position - _memoryTaken, data, n);
        } else {
            break;
        }
        position += (i64)n;
        data += n;
        kept += n;
    }
    return kept;
}

}  // namespace hbp
