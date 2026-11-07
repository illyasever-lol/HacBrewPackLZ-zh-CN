// Everything needed to write an NCA: the hash tree, the section layouts, the compression of a RomFS and the encrypted output.
#pragma once

#include <optional>

#include "crypto.h"
#include "nca.h"
#include "source.h"

namespace hbp {

/// What it takes in memory to rebuild this much game data, however well it compresses: the hash tree it comes with, the new one, and the compression
/// table. For a very large game, that's a good part of what there is.
i64 memoryToRebuild(i64 imageBytes, int blockSize);

struct IvfcResult {
    Bytes hashLevels;
    i64 levelOffsets[6];
    i64 levelSizes[6];
    u8 masterHash[32];
};

/// Builds the six level IVFC tree of a RomFS style section. Data is fed in stream order, and the hash levels are kept in memory
/// (about 1/512 of the data) until the data has gone by.
class IvfcBuilder {
public:
    static constexpr int BlockSize = 0x4000;
    static constexpr int BlockOrder = 14;

    IvfcBuilder();
    /// About this much is going to come. Having the room from the start means never having to move it, which takes twice the room.
    void expect(i64 dataBytes);
    void add(const u8* buffer, size_t count);
    /// The size recorded for the data level may exclude trailing padding that was hashed. The levels go ahead of the data unless it
    /// comes first, which is how something of a size that isn't known in advance can be written as it's made.
    IvfcResult finish(i64 dataSize, bool dataFirst = false);

    /// hacBrewPack always appends padding, even a whole block when the size is already aligned.
    static i64 padSize(i64 size) { return BlockSize - size % BlockSize; }
    /// The total size of the hash levels over a data level of this many bytes, as finish lays them out.
    static i64 hashLevelsSize(i64 dataSize);

private:
    void hashBlocks(const u8* buffer, size_t blocks);
    Bytes _partial;
    size_t _partialLength = 0;
    Bytes _hashes;
};

struct CompressedData {
    i64 virtualSize = 0;
    i64 tableOffset = 0;
    i64 tableSize = 0;
    i64 dataSize = 0;
    int entryCount = 0;
    IvfcResult ivfc;
};

/// Where plain bytes go, in order, as they're made.
class ByteSink {
public:
    virtual ~ByteSink() = default;
    virtual void write(const u8* data, size_t size) = 0;
};

/// Cuts an image into fixed size blocks, LZ4 compresses them in parallel and passes on the result plus its BKTR table. Blocks that
/// do not shrink are stored raw. Every compressed block is decoded again and compared with its input before it is accepted, so a
/// compressor fault cannot alter game data.
namespace RomFsCompressor {
/// A hand-over between threads that stops waiting once it's closed.
template <typename Item>
class Handover {
public:
    void add(Item* item) {
        sys::Lock lock(_mutex);
        _items.push_back(item);
        _wake.wakeOne();
    }
    /// Null once it's closed and empty.
    Item* take() {
        sys::Lock lock(_mutex);
        while (_items.empty() && !_closed)
            _wake.wait(_mutex);
        if (_items.empty())
            return nullptr;
        Item* item = _items.front();
        _items.erase(_items.begin());
        return item;
    }
    void close() {
        sys::Lock lock(_mutex);
        _closed = true;
        _wake.wakeAll();
    }

private:
    sys::Mutex _mutex;
    sys::CondVar _wake;
    std::vector<Item*> _items;
    bool _closed = false;
};

/// Average padding in front of a compressed block, from the 0x10 alignment.
constexpr int AveragePadding = 8;
/// A block has to save more than it costs: splitting a raw run adds table entries and padding, and every read has to decode it.
int compressCapacity(int length);
CompressedData run(const SourcePtr& image, ByteSink& payload, int blockSize, int level, int threads, Progress& progress);
}  // namespace RomFsCompressor

/// A worker per core, for jobs cut into many small pieces.
void parallelFor(int threads, int count, const std::function<void(int index, int worker)>& work);

struct NcaSection {
    Bytes fsHeader;
    SourcePtr data;
    bool encrypted = false;
    u64 upperIv = 0;
};

using NcaSections = std::vector<std::optional<NcaSection>>;

/// Section contents and FS headers for the two filesystem layouts an NCA can hold.
namespace Sections {
constexpr int ExefsHashBlockSize = 0x10000;
constexpr int LogoHashBlockSize = 0x1000;
constexpr int MetaHashBlockSize = 0x1000;

i64 pfs0SectionSize(i64 pfs0Size, int hashBlockSize);
i64 romFsSectionSize(i64 imageSize);
i64 compressedRomFsSectionSize(i64 storedSize, int tableEntries);
NcaSection pfs0(const SourcePtr& pfs0, int hashBlockSize, bool encrypted);
NcaSection romFs(const SourcePtr& image, bool encrypted, Progress* progress);
Bytes compressedRomFsHeader(const CompressedData& compressed, bool encrypted);
}  // namespace Sections

SourcePtr buildPfs0(const std::vector<std::pair<std::string, SourcePtr>>& files);

struct NcaInfo {
    u8 hash[32] = {};
    i64 size = 0;
    std::string id() const { return toHex(hash, 16); }
};

/// Where an NCA's bytes go.
class NcaStream {
public:
    virtual ~NcaStream() = default;
    virtual void write(const void* data, size_t size) = 0;
    /// Over what's already there. Offsets are from the start of the NCA.
    virtual void overwrite(i64 offset, const void* data, size_t size) = 0;
    /// What's been written so far, read back from where it went.
    virtual void readBack(i64 offset, void* data, size_t size) = 0;
    /// What to call reading it all back.
    virtual const char* readBackLabel() const { return "Checking what was written"; }
};

class NcaWriter {
public:
    static constexpr int HeaderRegionSize = 0xC00;

    NcaWriter(const u8* headerKey, const u8* keyAreaEncryptionKey, const u8* sectionKey, int keyGeneration);

    static i64 alignToSector(i64 size) { return (size + 0x1FF) & ~0x1FFLL; }
    static i64 totalSize(const NcaSections& sections);

    /// Everything has to be known by now, as the header goes first.
    NcaInfo write(NcaStream& output, NcaContentType contentType, u64 titleId, NcaSections& sections, bool sign, Progress* progress, int contentIndex, u32 sdkOverride);

private:
    friend class NcaBuilder;

    struct Place {
        bool used = false;
        i64 start = 0;
        i64 end = 0;
        Bytes fsHeader;
    };
    Bytes header(NcaContentType contentType, u64 titleId, const std::vector<Place>& places, i64 total, bool sign, int contentIndex, u32 sdkOverride) const;

    Bytes _headerKey;
    Bytes _keyAreaEncryptionKey;
    Bytes _sectionKey;
    int _keyGeneration;
};

/// Writes an NCA with a section compressed on its way there, so the compressed data never has to be anywhere but where it's going. As
/// the header says how it all came out, it goes in last, and the hash of the whole NCA can only be worked out after that. That takes
/// reading it all back, except for however much was kept in memory, and what's read back gets checked against what was written.
class NcaBuilder : private ByteSink {
public:
    /// Up to this much of the start of it is kept in memory, and doesn't have to be read back. What is read back only has to be, to work
    /// out its hash, and gets compared with what was written if asked.
    NcaBuilder(NcaWriter& writer, NcaStream& output, i64 holdInMemory, bool compareReadBack = true);
    void add(int index, NcaSection section);
    void addCompressed(int index, const SourcePtr& image, int blockSize, int level, int threads, bool encrypted, Progress& progress, CompressedData* details = nullptr);
    NcaInfo finish(NcaContentType contentType, u64 titleId, bool sign, int contentIndex, u32 sdkOverride);

private:
    static constexpr size_t ChunkSize = 4 << 20;
    static constexpr int DigestSize = 32;

    struct Written {
        u32 size;
        u8 digest[DigestSize];
    };

    void write(const u8* data, size_t size) override;
    void flush();
    void startSection(bool encrypted, u64 upperIv);
    void endSection(int index, Bytes fsHeader);

    NcaWriter& _writer;
    NcaStream& _output;
    i64 _holdLeft;
    bool _compare;
    AesCtr _ctr;
    bool _encrypted = false;
    u64 _upperIv = 0;
    std::unique_ptr<u8[]> _chunk;
    size_t _chunkUsed = 0;
    i64 _position;
    i64 _sectionStart = 0;
    std::vector<NcaWriter::Place> _places;
    std::vector<std::unique_ptr<u8[]>> _held;
    std::vector<Written> _written;
};

}  // namespace hbp
