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
constexpr int RewindsBeforeSpooling = 2;
constexpr int NcaHeaderRegionSize = 0xC00;

std::atomic<int> g_lastSweep{0};
thread_local int t_sweep = 0;
std::string g_spoolFolder;
std::atomic<int> g_spoolNumber{0};

}  // namespace

void NczSource::beginSinglePass() { t_sweep = ++g_lastSweep; }
void NczSource::endSinglePass() { t_sweep = 0; }
void NczSource::setSpoolFolder(const std::string& folder) { g_spoolFolder = folder; }

void NczSource::fail() const { throw ToolError(_name + "已损坏，或不是 NCZ"); }

NczSource::NczSource(SourcePtr file, std::string name) : _file(std::move(file)), _name(std::move(name)) {
    if (_file->length() < HeaderSize + 16)
        fail();

    _header.resize(HeaderSize);
    _file->read(0, _header.data(), HeaderSize);

    u8 head[16];
    _file->read(HeaderSize, head, sizeof(head));
    if (memcmp(head, "NCZSECTN", 8) != 0)
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
    if (!isSolid() || size <= 0 || size > MaxKeptSection)
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
            _spool->read(position, buffer, n);
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
    if (t_sweep == 0 && ++_rewinds > RewindsBeforeSpooling && !_spool) {
        std::string folder = g_spoolFolder.empty() ? std::string(".") : g_spoolFolder;
        sys::makeDirectories(folder);
        _spoolPath = format("%s/.hacbrewpacklz_%d.tmp", folder.c_str(), ++g_spoolNumber);
        _spool.reset(new sys::File(_spoolPath, sys::File::Mode::Create, _streamSize));
        _spooled = 0;
        sink().message(format("%s is needed out of order, so a temporary copy of up to %s is being kept in %s", _name.c_str(), formatSize(_streamSize).c_str(), folder.c_str()));
    }

    if (!_warned) {
        _warned = true;
        sink().message(_name + "使用固态压缩，必须从头重新解码，耗时较长");
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

    if (_spool && position == _spooled) {
        _spool->write(position, data, count);
        _spooled += (i64)count;
    }
}

}  // namespace hbp
