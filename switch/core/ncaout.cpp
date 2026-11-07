#include "ncaout.h"

#include <algorithm>
#include <exception>
#include <new>

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
    constexpr i64 PerBlock = sizeof(BucketTree::Entry) + BucketTree::EntrySize;
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

struct Batch {
    Batch(size_t capacity, size_t blocks) : input(new u8[capacity]), output(new u8[capacity]), outputLength(blocks) {}
    std::unique_ptr<u8[]> input;
    std::unique_ptr<u8[]> output;
    std::vector<int> outputLength;
    i64 virtualOffset = 0;
    size_t bytes = 0;
};

using BatchQueue = Handover<Batch>;

class PayloadWriter {
public:
    PayloadWriter(ByteSink& payload, i64 imageBytes, int blockSize) : _payload(payload) {
        _ivfc.expect(imageBytes);
        _entries.reserve((size_t)(imageBytes / blockSize + 1));
    }

    i64 position() const { return _position; }

    void addCompressed(i64 virtualOffset, const u8* buffer, int length) {
        align(PhysicalAlignment);
        _entries.push_back(BucketTree::Entry{virtualOffset, _position, 3, length});
        write(buffer, (size_t)length);
    }

    void addStored(i64 virtualOffset, const u8* buffer, int length) {
        // Consecutive stored blocks share one entry: raw entries are random accessible and have no size limit. As in retail NCAs,
        // a raw entry's physical size is -1, because the firmware never reads it.
        if (_storedRunEnd == _position && _storedRunSize + length <= MaxStoredRun) {
            _storedRunSize += length;
        } else {
            align(PhysicalAlignment);
            _entries.push_back(BucketTree::Entry{virtualOffset, _position, 0, -1});
            _storedRunSize = length;
        }
        write(buffer, (size_t)length);
        _storedRunEnd = _position;
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
};

}  // namespace

int compressCapacity(int length) { return length - std::max(length / 256, MinimumSaving); }

CompressedData run(const SourcePtr& image, ByteSink& payload, int blockSize, int level, int threads, Progress& progress) {
    int blocksPerBatch = std::max(threads * 32, 64) * (0x10000 / blockSize);
    size_t capacity = (size_t)blocksPerBatch * (size_t)blockSize;

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
            for (i64 position = 0; position < total;) {
                Batch* batch = free.take();
                if (!batch)
                    return;
                size_t n = (size_t)std::min<i64>(total - position, (i64)capacity);
                image->read(position, batch->input.get(), n);
                batch->virtualOffset = position;
                batch->bytes = n;
                position += (i64)n;
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
            while (Batch* batch = toWrite.take()) {
                size_t i = 0;
                for (size_t offset = 0; offset < batch->bytes; i++, offset += (size_t)blockSize) {
                    int length = (int)std::min((size_t)blockSize, batch->bytes - offset);
                    i64 virtualOffset = batch->virtualOffset + (i64)offset;
                    if (batch->outputLength[i] > 0)
                        writer.addCompressed(virtualOffset, batch->output.get() + offset, batch->outputLength[i]);
                    else
                        writer.addStored(virtualOffset, batch->input.get() + offset, length);
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
            int blocks = (int)((batch->bytes + (size_t)blockSize - 1) / (size_t)blockSize);
            parallelFor(threads, blocks, [&](int index, int worker) {
                size_t offset = (size_t)index * (size_t)blockSize;
                int length = (int)std::min((size_t)blockSize, batch->bytes - offset);
                int compressed = encoders[(size_t)worker]->compress(batch->input.get() + offset, length, batch->output.get() + offset, compressCapacity(length), level);
                if (compressed > 0) {
                    int decoded = lz4Decode(batch->output.get() + offset, compressed, checks[(size_t)worker].get(), Lz4Encoder::MaxBlockSize);
                    if (decoded != length || memcmp(checks[(size_t)worker].get(), batch->input.get() + offset, (size_t)length) != 0)
                        throw ToolError(format("Internal error: LZ4 round trip failed at RomFS offset 0x%llx", (long long)(batch->virtualOffset + (i64)offset)));
                }
                batch->outputLength[(size_t)index] = std::max(compressed, 0);
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

void NcaBuilder::addCompressed(int index, const SourcePtr& image, int blockSize, int level, int threads, bool encrypted, Progress& progress, CompressedData* details) {
    startSection(encrypted, 0);
    CompressedData data = RomFsCompressor::run(image, *this, blockSize, level, threads, progress);
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
        Progress progress(_compare ? _output.readBackLabel() : "正在计算其哈希", _position - position);
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
