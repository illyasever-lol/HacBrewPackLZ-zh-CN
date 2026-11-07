#include "nca.h"

#include <algorithm>

#include "lz4.h"

namespace hbp {

// -------------------------------------------------------------------------------------------------------------------
// BucketTree

namespace BucketTree {

static constexpr u32 Magic = 0x52544B42;  // "BKTR"
static constexpr u32 Version = 1;

static int divideUp(int value, int divisor) { return (value + divisor - 1) / divisor; }

int entrySetCount(int entries, int entrySize) { return divideUp(entries, (NodeSize - NodeHeaderSize) / entrySize); }

int l2NodeCount(int entries, int entrySize) {
    int sets = entrySetCount(entries, entrySize);
    if (sets <= OffsetsPerNode)
        return 0;
    int estimate = divideUp(sets, OffsetsPerNode);
    if (estimate > OffsetsPerNode)
        throw ToolError("此游戏需要的压缩表项过多：请增大块大小");
    return divideUp(sets - (OffsetsPerNode - (estimate - 1)), OffsetsPerNode);
}

i64 nodeStorageSize(int entries, int entrySize) { return (1 + l2NodeCount(entries, entrySize)) * (i64)NodeSize; }
i64 entryStorageSize(int entries, int entrySize) { return entrySetCount(entries, entrySize) * (i64)NodeSize; }

int readHeader(const u8* header, const std::string& what) {
    if (rd32(header) != Magic || rd32(header + 4) > Version)
        throw ToolError(what + "的 BKTR 表头无效");
    int entries = rdi32(header + 8);
    if (entries <= 0)
        throw ToolError(what + "的 BKTR 表为空");
    return entries;
}

i64 enumerateEntries(const Bytes& table, int entrySize, int entryCount, const std::string& what, const std::function<void(int, const u8*)>& visit) {
    i64 entryStorage = nodeStorageSize(entryCount, entrySize);
    int sets = entrySetCount(entryCount, entrySize);
    if (entryStorage + sets * (i64)NodeSize > (i64)table.size())
        throw ToolError(what + "的 BKTR 表小于其条目数所需大小");

    int perSet = (NodeSize - NodeHeaderSize) / entrySize;
    int seen = 0;
    i64 end = 0;
    for (int set = 0; set < sets; set++) {
        const u8* node = table.data() + entryStorage + set * (i64)NodeSize;
        int index = rdi32(node);
        int count = rdi32(node + 4);
        i64 setEnd = rdi64(node + 8);
        if (index != set || count <= 0 || count > perSet || seen + count > entryCount || setEnd < end)
            throw ToolError(what + "的 BKTR 条目集已损坏");
        for (int i = 0; i < count; i++)
            visit(seen + i, node + NodeHeaderSize + i * entrySize);
        seen += count;
        end = setEnd;
    }
    if (seen != entryCount)
        throw ToolError(format("%s has %d BKTR entries, its header says %d", what.c_str(), seen, entryCount));
    return end;
}

static void writeNodeHeader(u8* node, int index, int count, i64 endOffset) {
    wr32(node, (u32)index);
    wr32(node + 4, (u32)count);
    wr64(node + 8, (u64)endOffset);
}

static void putOffset(u8* node, int index, i64 offset) { wr64(node + NodeHeaderSize + index * 8, (u64)offset); }

Bytes build(const std::vector<Entry>& entries, i64 virtualSize) {
    int total = (int)entries.size();
    int sets = entrySetCount(total);
    int l2Nodes = l2NodeCount(total);
    Bytes table((size_t)(nodeStorageSize(total) + entryStorageSize(total)));
    u8* entryStorage = table.data() + (1 + l2Nodes) * NodeSize;

    std::vector<i64> setStarts((size_t)sets);
    for (int set = 0; set < sets; set++) {
        int first = set * EntriesPerSet;
        int count = std::min(EntriesPerSet, total - first);
        i64 end = first + count < total ? entries[(size_t)(first + count)].virtualOffset : virtualSize;
        setStarts[(size_t)set] = entries[(size_t)first].virtualOffset;

        u8* node = entryStorage + set * NodeSize;
        writeNodeHeader(node, set, count, end);
        for (int i = 0; i < count; i++) {
            u8* e = node + NodeHeaderSize + i * EntrySize;
            const Entry& entry = entries[(size_t)(first + i)];
            wr64(e, (u64)entry.virtualOffset);
            wr64(e + 8, (u64)entry.physicalOffset);
            e[0x10] = entry.type;
            wr32(e + 0x14, (u32)entry.physicalSize);
        }
    }

    u8* l1 = table.data();
    if (l2Nodes == 0) {
        writeNodeHeader(l1, 0, sets, virtualSize);
        for (int set = 0; set < sets; set++)
            putOffset(l1, set, setStarts[(size_t)set]);
        return table;
    }

    // L1 starts with one offset per L2 node; its remaining slots point straight at the first entry sets.
    int setsOnL1 = OffsetsPerNode - l2Nodes;
    writeNodeHeader(l1, 0, l2Nodes, virtualSize);
    for (int i = 0; i < setsOnL1; i++)
        putOffset(l1, l2Nodes + i, setStarts[(size_t)i]);

    for (int n = 0; n < l2Nodes; n++) {
        int first = setsOnL1 + n * OffsetsPerNode;
        int count = std::min(OffsetsPerNode, sets - first);
        i64 end = first + count < sets ? setStarts[(size_t)(first + count)] : virtualSize;
        u8* node = table.data() + (1 + n) * NodeSize;
        writeNodeHeader(node, n, count, end);
        for (int i = 0; i < count; i++)
            putOffset(node, i, setStarts[(size_t)(first + i)]);
        putOffset(l1, n, setStarts[(size_t)first]);
    }
    return table;
}

void writeHeader(u8* destination, int entries) {
    wr32(destination, Magic);
    wr32(destination + 4, Version);
    wr32(destination + 8, (u32)entries);
    wr32(destination + 12, 0);
}

}  // namespace BucketTree

// -------------------------------------------------------------------------------------------------------------------
// Tickets

void TicketStore::addTicket(const u8* ticket, size_t size) {
    if (size < 4)
        return;
    int bodyOffset;
    switch (rd32(ticket)) {
        case 0x10000:
        case 0x10003:
            bodyOffset = 4 + 0x200 + 0x3C;
            break;
        case 0x10001:
        case 0x10004:
            bodyOffset = 4 + 0x100 + 0x3C;
            break;
        case 0x10002:
        case 0x10005:
            bodyOffset = 4 + 0x3C + 0x40;
            break;
        default:
            return;
    }
    if (size < (size_t)bodyOffset + 0x170)
        return;

    const u8* body = ticket + bodyOffset;
    std::string rightsId = toHex(body + 0x160, 0x10);
    if (body[0x141] == 0)
        _encryptedKeys[rightsId].assign(body + 0x40, body + 0x50);
    else
        _personalized.insert(rightsId);
}

void TicketStore::addEncryptedKey(const std::string& rightsIdHex, const u8 key[16]) {
    _encryptedKeys[rightsIdHex].assign(key, key + 16);
    _personalized.erase(rightsIdHex);
}

Bytes TicketStore::titleKey(const u8* rightsId, int generationIndex, const std::string& ncaName) const {
    std::string id = toHex(rightsId, 0x10);
    auto encrypted = _encryptedKeys.find(id);
    if (encrypted != _encryptedKeys.end()) {
        const u8* kek = _keyset.titleKek(generationIndex);
        if (!kek)
            throw ToolError(format("titlekek_%02x is not in your keys, it is needed for %s. Your keys may be older than this game", generationIndex, ncaName.c_str()));
        Bytes key = encrypted->second;
        aesEcbDecrypt(kek, key.data(), key.size());
        return key;
    }
    if (const u8* plain = _keyset.titleKey(id))
        return Bytes(plain, plain + 16);
    if (_personalized.count(id))
        throw ToolError(ncaName + " uses a personalized ticket tied to one console, and its key couldn't be worked out. Add it to title.keys beside your keys");
    throw ToolError(format("%s needs a title key, but there is no ticket for rights ID %s. Add its key to title.keys beside your keys", ncaName.c_str(), id.c_str()));
}

// -------------------------------------------------------------------------------------------------------------------
// The layers

namespace {

class CtrDecryptor {
public:
    CtrDecryptor(SourcePtr raw, const u8* key, i64 sectionFileOffset) : _raw(std::move(raw)), _ctr(key), _sectionFileOffset(sectionFileOffset) {}

    void read(i64 offset, u8* buffer, size_t count, u64 upperIv) {
        i64 aligned = offset & ~15LL;
        size_t head = (size_t)(offset - aligned);
        size_t total = (head + count + 15) & ~(size_t)15;
        if (head == 0 && total == count) {
            _raw->read(offset, buffer, count);
            _ctr.crypt(buffer, count, _sectionFileOffset + offset, upperIv);
            return;
        }
        Bytes scratch(total);
        _raw->read(aligned, scratch.data(), total);
        _ctr.crypt(scratch.data(), total, _sectionFileOffset + aligned, upperIv);
        memcpy(buffer, scratch.data() + head, count);
    }

    bool readsAtOnce() const { return _raw->readsAtOnce(); }

private:
    SourcePtr _raw;
    AesCtr _ctr;
    i64 _sectionFileOffset;
};

class AesCtrSource : public DataSource {
public:
    AesCtrSource(const SourcePtr& raw, const u8* key, i64 sectionFileOffset, u64 upperIv) : _length(raw->length()), _decryptor(raw, key, sectionFileOffset), _upperIv(upperIv) {}
    i64 length() const override { return _length; }
    void read(i64 offset, void* buffer, size_t count) override { _decryptor.read(offset, (u8*)buffer, count, _upperIv); }
    bool readsAtOnce() const override { return _decryptor.readsAtOnce(); }

private:
    i64 _length;
    CtrDecryptor _decryptor;
    u64 _upperIv;
};

/// Finds the last start at or before value. Sequential reads usually hit the hint or the entry after it.
int findEntry(const std::vector<i64>& starts, i64 end, i64 value, int hint) {
    if (value < 0 || value >= end || starts.empty() || value < starts[0])
        return -1;
    int size = (int)starts.size();
    if (hint < size && starts[(size_t)hint] <= value) {
        if (hint + 1 == size || value < starts[(size_t)hint + 1])
            return hint;
        if (hint + 2 == size || value < starts[(size_t)hint + 2])
            return hint + 1;
    }
    return (int)(std::upper_bound(starts.begin(), starts.end(), value) - starts.begin()) - 1;
}

/// An update's own data: each region has its own counter value, listed in the AES-CTR-Ex table.
class AesCtrExSource : public DataSource {
public:
    AesCtrExSource(SourcePtr raw, const u8* key, i64 sectionFileOffset, u32 secureValue, const Bytes& table, const u8* treeHeader, std::string name)
        : _raw(raw), _decryptor(raw, key, sectionFileOffset), _secureValue(secureValue), _name(std::move(name)) {
        int count = BucketTree::readHeader(treeHeader, _name);
        _offsets.resize((size_t)count);
        _generations.resize((size_t)count);
        _encrypted.resize((size_t)count);
        _end = BucketTree::enumerateEntries(table, 0x10, count, _name, [this](int i, const u8* e) {
            _offsets[(size_t)i] = rdi64(e);
            _encrypted[(size_t)i] = e[8] == 0;
            _generations[(size_t)i] = rdi32(e + 0xC);
        });
    }

    i64 length() const override { return _raw->length(); }
    bool readsAtOnce() const override { return _raw->readsAtOnce(); }

    void read(i64 offset, void* buffer, size_t count) override {
        u8* out = (u8*)buffer;
        while (count > 0) {
            int found = findEntry(_offsets, _end, offset, _cursor.load(std::memory_order_relaxed));
            if (found < 0)
                throw ToolError(format("%s: offset 0x%llx isn't covered by the AES-CTR-Ex table", _name.c_str(), (long long)offset));
            _cursor.store(found, std::memory_order_relaxed);
            size_t entry = (size_t)found;
            i64 entryEnd = entry + 1 < _offsets.size() ? _offsets[entry + 1] : _end;
            size_t n = (size_t)std::min<i64>((i64)count, entryEnd - offset);
            if (_encrypted[entry])
                _decryptor.read(offset, out, n, (u64)_secureValue << 32 | (u32)_generations[entry]);
            else
                _raw->read(offset, out, n);
            offset += (i64)n;
            out += n;
            count -= n;
        }
    }

private:
    SourcePtr _raw;
    CtrDecryptor _decryptor;
    u32 _secureValue;
    std::vector<i64> _offsets;
    std::vector<int> _generations;
    std::vector<char> _encrypted;
    i64 _end = 0;
    std::string _name;
    std::atomic<int> _cursor{0};
};

/// An update's view of a section: every range comes either from the base game's section or from the update.
class IndirectSource : public DataSource {
public:
    IndirectSource(SourcePtr original, SourcePtr patch, const Bytes& table, const u8* treeHeader, std::string name)
        : _original(std::move(original)), _patch(std::move(patch)), _name(std::move(name)) {
        int count = BucketTree::readHeader(treeHeader, _name);
        _virtual.resize((size_t)count);
        _physical.resize((size_t)count);
        _fromPatch.resize((size_t)count);
        _end = BucketTree::enumerateEntries(table, 0x14, count, _name, [this](int i, const u8* e) {
            _virtual[(size_t)i] = rdi64(e);
            _physical[(size_t)i] = rdi64(e + 8);
            _fromPatch[(size_t)i] = rdi32(e + 0x10) != 0;
        });
    }

    i64 length() const override { return _end; }
    bool readsAtOnce() const override { return _patch->readsAtOnce() && (!_original || _original->readsAtOnce()); }

    void read(i64 offset, void* buffer, size_t count) override {
        u8* out = (u8*)buffer;
        while (count > 0) {
            int found = findEntry(_virtual, _end, offset, _cursor.load(std::memory_order_relaxed));
            if (found < 0)
                throw ToolError(format("%s: offset 0x%llx isn't covered by the update's table", _name.c_str(), (long long)offset));
            _cursor.store(found, std::memory_order_relaxed);
            size_t entry = (size_t)found;
            i64 entryEnd = entry + 1 < _virtual.size() ? _virtual[entry + 1] : _end;
            size_t n = (size_t)std::min<i64>((i64)count, entryEnd - offset);
            i64 source = _physical[entry] + (offset - _virtual[entry]);
            if (_fromPatch[entry])
                _patch->read(source, out, n);
            else if (_original)
                _original->read(source, out, n);
            else
                throw ToolError(_name + "需要其本体游戏，但未提供");

            offset += (i64)n;
            out += n;
            count -= n;
        }
    }

private:
    SourcePtr _original;
    SourcePtr _patch;
    std::vector<i64> _virtual;
    std::vector<i64> _physical;
    std::vector<char> _fromPatch;
    i64 _end = 0;
    std::string _name;
    std::atomic<int> _cursor{0};
};

/// A section of a base game that's been cut down to what its update still reads from it. What's left is kept together, encrypted as it was where
/// it used to be, so to anything reading it this looks like the whole section, with nothing where the rest used to be.
class SparseSource : public DataSource {
public:
    SparseSource(SourcePtr raw, i64 dataStart, i64 length, const Bytes& table, const u8* treeHeader, std::string name) : _raw(std::move(raw)), _dataStart(dataStart), _length(length), _name(std::move(name)) {
        int count = BucketTree::readHeader(treeHeader, _name);
        _virtual.resize((size_t)count);
        _physical.resize((size_t)count);
        _present.resize((size_t)count);
        _end = BucketTree::enumerateEntries(table, 0x14, count, _name, [this](int i, const u8* e) {
            _virtual[(size_t)i] = rdi64(e);
            _physical[(size_t)i] = rdi64(e + 8);
            _present[(size_t)i] = rdi32(e + 0x10) == 0;
        });
    }

    i64 length() const override { return _length; }
    bool readsAtOnce() const override { return _raw->readsAtOnce(); }

    void read(i64 offset, void* buffer, size_t count) override {
        u8* out = (u8*)buffer;
        while (count > 0) {
            int found = findEntry(_virtual, _end, offset, _cursor.load(std::memory_order_relaxed));
            if (found < 0) {
                memset(out, 0, count);
                return;
            }
            _cursor.store(found, std::memory_order_relaxed);
            size_t entry = (size_t)found;
            i64 entryEnd = entry + 1 < _virtual.size() ? _virtual[entry + 1] : _end;
            size_t n = (size_t)std::min<i64>((i64)count, entryEnd - offset);
            if (_present[entry])
                _raw->read(_dataStart + _physical[entry] + (offset - _virtual[entry]), out, n);
            else
                memset(out, 0, n);
            offset += (i64)n;
            out += n;
            count -= n;
        }
    }

private:
    SourcePtr _raw;
    i64 _dataStart;
    i64 _length;
    std::vector<i64> _virtual;
    std::vector<i64> _physical;
    std::vector<char> _present;
    i64 _end = 0;
    std::string _name;
    std::atomic<int> _cursor{0};
};

constexpr int HashSize = 0x20;
constexpr int WindowSize = 1 << 22;

/// A short last block is hashed with zeros up to the block size, as the firmware does.
void checkBlocks(u8* buffer, size_t capacity, size_t length, i64 position, const Bytes& hashes, int blockSize, const std::string& what, const std::string& hint) {
    size_t blocks = (length + (size_t)blockSize - 1) / (size_t)blockSize;
    i64 firstBlock = position / blockSize;
    if ((firstBlock + (i64)blocks) * HashSize > (i64)hashes.size())
        throw ToolError(what + "：哈希表短于其数据所需长度，文件已损坏" + hint);

    if (length % (size_t)blockSize != 0) {
        size_t padding = (size_t)blockSize - length % (size_t)blockSize;
        if (length + padding > capacity)
            throw ToolError("内部错误：哈希缓冲区过小");
        memset(buffer + length, 0, padding);
    }

    for (size_t i = 0; i < blocks; i++) {
        u8 hash[HashSize];
        sha256(buffer + i * (size_t)blockSize, (size_t)blockSize, hash);
        if (memcmp(hash, hashes.data() + (firstBlock + (i64)i) * HashSize, HashSize) != 0)
            throw ToolError(format("%s: hash check failed at offset 0x%llx. The file is damaged%s", what.c_str(), (long long)((firstBlock + (i64)i) * blockSize), hint.c_str()));
    }
}

/// The last level of a hash tree, checking whole blocks against their hashes as they are read.
class VerifiedSource : public DataSource {
public:
    VerifiedSource(SourcePtr data, Bytes hashes, int blockSize, std::string what, std::string hint)
        : _data(std::move(data)), _hashes(std::move(hashes)), _blockSize(blockSize), _what(std::move(what)), _hint(std::move(hint)), _windowCapacity((size_t)std::max(WindowSize, blockSize)), _window(new u8[_windowCapacity]) {}

    i64 length() const override { return _data->length(); }

    void read(i64 offset, void* buffer, size_t count) override {
        if (offset < 0 || offset + (i64)count > _data->length())
            throw ToolError("内部错误：读取超出已校验数据范围");
        u8* out = (u8*)buffer;
        while (count > 0) {
            if (_windowStart < 0 || offset < _windowStart || offset >= _windowStart + (i64)_windowLength)
                loadWindow(offset);
            size_t inWindow = (size_t)(offset - _windowStart);
            size_t n = std::min(count, _windowLength - inWindow);
            memcpy(out, _window.get() + inWindow, n);
            offset += (i64)n;
            out += n;
            count -= n;
        }
    }

private:
    void loadWindow(i64 offset) {
        i64 start = offset / _blockSize * _blockSize;
        size_t length = (size_t)std::min<i64>((i64)(_windowCapacity / (size_t)_blockSize * (size_t)_blockSize), _data->length() - start);
        _data->read(start, _window.get(), length);
        checkBlocks(_window.get(), _windowCapacity, length, start, _hashes, _blockSize, _what, _hint);
        _windowStart = start;
        _windowLength = length;
    }

    SourcePtr _data;
    Bytes _hashes;
    int _blockSize;
    std::string _what;
    std::string _hint;
    size_t _windowCapacity;
    std::unique_ptr<u8[]> _window;
    i64 _windowStart = -1;
    size_t _windowLength = 0;
};

/// A compressed RomFS seen as its plain image. Accepts everything the firmware's reader does, not only this tool's output.
class CompressedImageSource : public DataSource {
public:
    CompressedImageSource(SourcePtr data, const Bytes& table, const u8* treeHeader, std::string name) : _data(std::move(data)), _name(std::move(name)), _compressed(0x10000), _block(0x10000) {
        int count = BucketTree::readHeader(treeHeader, _name);
        _virtual.resize((size_t)count);
        _physical.resize((size_t)count);
        _physicalSize.resize((size_t)count);
        _types.resize((size_t)count);
        _end = BucketTree::enumerateEntries(table, 0x18, count, _name, [this](int i, const u8* e) {
            _virtual[(size_t)i] = rdi64(e);
            _physical[(size_t)i] = rdi64(e + 8);
            _types[(size_t)i] = e[0x10];
            _physicalSize[(size_t)i] = rdi32(e + 0x14);
        });
    }

    i64 length() const override { return _end; }

    void read(i64 offset, void* buffer, size_t count) override {
        u8* out = (u8*)buffer;
        while (count > 0) {
            _cursor = findEntry(_virtual, _end, offset, _cursor);
            if (_cursor < 0)
                throw ToolError(format("%s: offset 0x%llx isn't covered by the compression table", _name.c_str(), (long long)offset));

            size_t entry = (size_t)_cursor;
            i64 entryEnd = entry + 1 < _virtual.size() ? _virtual[entry + 1] : _end;
            i64 inEntry = offset - _virtual[entry];
            size_t n = (size_t)std::min<i64>((i64)count, entryEnd - offset);
            switch (_types[entry]) {
                case 0:
                    _data->read(_physical[entry] + inEntry, out, n);
                    break;
                case 1:
                    memset(out, 0, n);
                    break;
                case 3:
                    if (entryEnd - _virtual[entry] > INT32_MAX)
                        throw ToolError(_name + "含有一个过大的压缩块");
                    loadBlock(_cursor, (int)(entryEnd - _virtual[entry]));
                    memcpy(out, _block.data() + inEntry, n);
                    break;
                default:
                    throw ToolError(format("%s uses compression type %d, which isn't supported", _name.c_str(), _types[entry]));
            }
            offset += (i64)n;
            out += n;
            count -= n;
        }
    }

private:
    void loadBlock(int entry, int virtualSize) {
        if (_blockEntry == entry)
            return;
        int physicalSize = _physicalSize[(size_t)entry];
        if (physicalSize <= 0)
            throw ToolError(format("%s: compressed block at 0x%llx has an invalid size", _name.c_str(), (long long)_virtual[(size_t)entry]));
        if ((int)_compressed.size() < physicalSize)
            _compressed.resize((size_t)physicalSize);
        if ((int)_block.size() < virtualSize)
            _block.resize((size_t)virtualSize);

        _blockEntry = -1;
        _data->read(_physical[(size_t)entry], _compressed.data(), (size_t)physicalSize);
        if (lz4Decode(_compressed.data(), physicalSize, _block.data(), virtualSize) != virtualSize)
            throw ToolError(format("%s: compressed block at 0x%llx failed to decode", _name.c_str(), (long long)_virtual[(size_t)entry]));
        _blockEntry = entry;
    }

    SourcePtr _data;
    std::vector<i64> _virtual;
    std::vector<i64> _physical;
    std::vector<int> _physicalSize;
    std::vector<u8> _types;
    i64 _end = 0;
    std::string _name;
    Bytes _compressed;
    Bytes _block;
    int _blockEntry = -1;
    int _cursor = 0;
};

}  // namespace

// -------------------------------------------------------------------------------------------------------------------
// NcaFile

NcaFile::NcaFile(SourcePtr raw, std::string name, const Keyset& keyset, const TicketStore& tickets)
    : _raw(std::move(raw)), _ncz(dynamic_cast<NczSource*>(_raw.get())), _keyset(keyset), _tickets(tickets), _name(std::move(name)) {
    if (_raw->length() < 0xC00)
        throw ToolError(format("%s is only %lld bytes, where an NCA takes at least 3 KB", _name.c_str(), (long long)_raw->length()));

    _header.resize(0xC00);
    _raw->read(0, _header.data(), _header.size());
    aesXts(keyset.headerKey(), _header.data(), _header.size(), false);
    if (memcmp(&_header[0x200], "NCA3", 4) != 0)
        throw ToolError(_name + "不是可读的 NCA3，可能已损坏，或 header_key 有误");

    for (int i = 0; i < SectionCount; i++) {
        i64 start = rd32(&_header[0x240 + (size_t)i * 0x10]) * 0x200LL;
        i64 end = rd32(&_header[0x244 + (size_t)i * 0x10]) * 0x200LL;
        if (start == 0 && end == 0)
            continue;

        auto section = std::make_unique<NcaFsSection>();
        section->index = i;
        section->offset = start;
        section->size = end - start;
        section->header.assign(_header.begin() + 0x400 + i * 0x200, _header.begin() + 0x400 + (i + 1) * 0x200);
        u8 hash[32];
        sha256(section->header.data(), section->header.size(), hash);
        if (memcmp(hash, &_header[0x280 + (size_t)i * 0x20], 32) != 0)
            throw ToolError(format("%s section %d has a damaged FS header", _name.c_str(), i));
        // A section that's been cut down still says where it used to be, which can be well past the end of the file.
        if (start < 0xC00 || end < start || (end > _raw->length() && !section->hasSparse()))
            throw ToolError(format("%s section %d lies outside the file", _name.c_str(), i));
        _sections[i] = std::move(section);
        if (_ncz)
            _ncz->keepIfSmall(start, end - start);
    }
}

int NcaFile::generationIndex() const { return std::max((int)std::max(_header[0x206], _header[0x220]) - 1, 0); }

bool NcaFile::hasRightsId() const {
    for (int i = 0; i < 0x10; i++) {
        if (_header[0x230 + (size_t)i] != 0)
            return true;
    }
    return false;
}

bool NcaFile::holdsOnlyPart() const {
    for (auto& section : _sections) {
        if (section && section->hasSparse())
            return true;
    }
    return false;
}

const Bytes& NcaFile::contentKey() {
    if (!_contentKey.empty())
        return _contentKey;
    if (hasRightsId()) {
        _contentKey = _tickets.titleKey(&_header[0x230], generationIndex(), _name);
        return _contentKey;
    }

    constexpr int KeyIndexCtr = 2;
    int keyAreaIndex = _header[0x207];
    const u8* kaek = _keyset.keyAreaKey(keyAreaIndex, generationIndex());
    if (!kaek)
        throw ToolError(format("The key area key for generation %02x is not in your keys, it is needed for %s. Your keys may be older than this game", generationIndex(), _name.c_str()));
    Bytes keyArea(_header.begin() + 0x300, _header.begin() + 0x340);
    aesEcbDecrypt(kaek, keyArea.data(), keyArea.size());
    _contentKey.assign(keyArea.begin() + KeyIndexCtr * 0x10, keyArea.begin() + (KeyIndexCtr + 1) * 0x10);
    return _contentKey;
}

SourcePtr NcaFile::openSparse(const NcaFsSection& section) {
    if (_ncz)
        throw ToolError(format("%s section %d is only there in part, which can't be read from an NCZ", _name.c_str(), section.index));
    // With nothing listed, none of it is here at all, and the update has all of it.
    if (rdi32(section.sparseTableHeader() + 8) == 0)
        return std::make_shared<ZeroSource>(section.size);

    i64 start = section.sparseDataOffset() + section.sparseTableOffset(), size = section.sparseTableSize();
    if (section.sparseDataOffset() < 0 || section.sparseTableOffset() < 0 || size <= 0 || start + size > _raw->length() || size > INT32_MAX)
        throw ToolError(format("%s section %d has a sparse table outside the file", _name.c_str(), section.index));

    // Its table is encrypted where it now is, with a counter that says which time this section was cut down.
    SourcePtr raw = std::make_shared<SubSource>(_raw, start, size);
    if (section.encryption() != NcaFsSection::EncryptNone)
        raw = std::make_shared<AesCtrSource>(raw, contentKey().data(), start, (u64)section.secureValue() << 32 | (u32)section.sparseGeneration() << 16);
    Bytes table((size_t)size);
    raw->read(0, table.data(), table.size());
    return std::make_shared<SparseSource>(_raw, section.sparseDataOffset(), section.size, table, section.sparseTableHeader(), _name);
}

SourcePtr NcaFile::openPhysical(const NcaFsSection& section) {
    // An NCZ holds its sections decrypted, so neither keys nor tickets come into it.
    SourcePtr raw = section.hasSparse() ? openSparse(section) : std::make_shared<SubSource>(_raw, section.offset, section.size);
    if (_ncz)
        return raw;

    switch (section.encryption()) {
        case NcaFsSection::EncryptNone:
            return raw;
        case NcaFsSection::EncryptCtr:
            return std::make_shared<AesCtrSource>(raw, contentKey().data(), section.offset, section.upperIv());
        case NcaFsSection::EncryptCtrEx: {
            // An update that changes nothing in this section has none of its own data in it, and so nothing for the table to say.
            if (section.ctrExSize() == 0)
                return std::make_shared<AesCtrSource>(raw, contentKey().data(), section.offset, section.upperIv());
            Bytes table = readMetaTable(section, section.ctrExOffset(), section.ctrExSize());
            return std::make_shared<AesCtrExSource>(raw, contentKey().data(), section.offset, section.secureValue(), table, section.ctrExHeader(), _name);
        }
        default:
            throw ToolError(format("%s section %d uses encryption type %d, which isn't supported", _name.c_str(), section.index, section.encryption()));
    }
}

SourcePtr NcaFile::openLogical(const NcaFsSection& section, NcaFile* baseNca) {
    if (section.hasSparse())
        throw ToolError(format("%s only holds part of the game, and the rest of it is in its update", _name.c_str()));
    SourcePtr physical = openPhysical(section);
    if (!section.hasIndirect())
        return physical;

    SourcePtr original;
    if (baseNca && baseNca->section(section.index))
        original = baseNca->openPhysical(*baseNca->section(section.index));
    Bytes table = readMetaTable(section, section.indirectOffset(), section.indirectSize());
    return std::make_shared<IndirectSource>(original, physical, table, section.indirectHeader(), _name);
}

SourcePtr NcaFile::openRomFsImage(const NcaFsSection& section, NcaFile* baseNca, bool verify) {
    if (section.fsType() != NcaFsSection::FsRomFs)
        throw ToolError(format("%s section %d is not a RomFS", _name.c_str(), section.index));

    SourcePtr logical = openLogical(section, baseNca);
    std::string hint = section.hasIndirect() ? "，或者该升级补丁不属于此本体游戏" : "";
    std::string what = format("%s section %d", _name.c_str(), section.index);
    if (section.hashType() != NcaFsSection::HashIntegrity && section.hashType() != NcaFsSection::HashIntegritySha3)
        throw ToolError(format("%s has a RomFS with hash type %d, which isn't supported", what.c_str(), section.hashType()));
    SourcePtr data = openIntegrityData(section, logical, what, hint, verify);
    return section.hasCompression() ? openCompressed(section, data) : data;
}

std::unique_ptr<Pfs0Archive> NcaFile::openPfs0(const NcaFsSection& section, NcaFile* baseNca, bool verify, i64 keepInMemory) {
    SourcePtr logical = openLogical(section, baseNca);
    if (logical->length() <= keepInMemory)
        logical = std::make_shared<WholeInMemory>(logical);
    return Pfs0Archive::open(logical, section, format("%s section %d", _name.c_str(), section.index), verify);
}

/// The update tables always use plain AES-CTR with the section's own counter value.
Bytes NcaFile::readMetaTable(const NcaFsSection& section, i64 offset, i64 size) {
    if (offset < 0 || size <= 0 || offset + size > section.size || size > INT32_MAX)
        throw ToolError(format("%s section %d has an update table outside the section", _name.c_str(), section.index));

    SourcePtr raw = std::make_shared<SubSource>(_raw, section.offset, section.size);
    if (section.encryption() != NcaFsSection::EncryptNone && !_ncz)
        raw = std::make_shared<AesCtrSource>(raw, contentKey().data(), section.offset, section.upperIv());
    Bytes table((size_t)size);
    raw->read(offset, table.data(), table.size());
    return table;
}

SourcePtr NcaFile::openIntegrityData(const NcaFsSection& section, SourcePtr logical, const std::string& what, const std::string& hint, bool wantVerify) {
    const u8* h = section.header.data();
    if (memcmp(h + 0x08, "IVFC", 4) != 0)
        throw ToolError(what + "没有 IVFC 头");
    int layers = rdi32(h + 0x14);
    if (layers < 2 || layers > 7)
        throw ToolError(format("%s has %d IVFC layers", what.c_str(), layers));

    // SHA-3 trees exist in the format, but aren't worth carrying an implementation for: read them unchecked.
    bool verify = wantVerify && section.hashType() == NcaFsSection::HashIntegrity;
    if (wantVerify && !verify)
        sink().warning(what + "使用 SHA-3 哈希树，因此无法校验其数据");

    Bytes hashes(h + 0xC8, h + 0xE8);
    int levels = layers - 1;
    for (int level = 0; level < levels; level++) {
        const u8* info = h + 0x18 + level * 0x18;
        i64 offset = rdi64(info);
        i64 size = rdi64(info + 8);
        int order = rdi32(info + 0x10);
        if (offset < 0 || size < 0 || offset + size > logical->length() || order < 9 || order > 24)
            throw ToolError(format("%s has an invalid IVFC level %d", what.c_str(), level));

        if (level == levels - 1) {
            SourcePtr data = std::make_shared<SubSource>(logical, offset, size);
            if (!verify)
                return data;
            return std::make_shared<VerifiedSource>(data, std::move(hashes), 1 << order, what, hint);
        }
        if (!verify)
            continue;

        int blockSize = 1 << order;
        Bytes bytes((size_t)size);
        logical->read(offset, bytes.data(), bytes.size());
        // Checked in a copy, so the padding a short last block gets doesn't end up in the hashes of the next level down.
        Bytes padded((size_t)((size + blockSize - 1) / blockSize * blockSize));
        memcpy(padded.data(), bytes.data(), bytes.size());
        checkBlocks(padded.data(), padded.size(), bytes.size(), 0, hashes, blockSize, what, hint);
        hashes = std::move(bytes);
    }
    throw ToolError("内部错误：没有 IVFC 数据层");
}

SourcePtr NcaFile::openCompressed(const NcaFsSection& section, SourcePtr data) {
    i64 offset = section.compressionOffset(), size = section.compressionSize();
    if (offset < 0 || size <= 0 || offset + size > data->length() || size > INT32_MAX)
        throw ToolError(format("%s section %d has a compression table outside its data", _name.c_str(), section.index));

    Bytes table((size_t)size);
    data->read(offset, table.data(), table.size());
    return std::make_shared<CompressedImageSource>(std::make_shared<SubSource>(data, 0, offset), table, section.compressionHeader(), _name);
}

// -------------------------------------------------------------------------------------------------------------------
// PFS0 and RomFS

Bytes Pfs0Archive::readAll(const Entry& entry) const {
    Bytes bytes((size_t)entry.size);
    _data->read(entry.offset, bytes.data(), bytes.size());
    return bytes;
}

std::unique_ptr<Pfs0Archive> Pfs0Archive::open(SourcePtr logical, const NcaFsSection& section, const std::string& what, bool verify) {
    if (section.fsType() != NcaFsSection::FsPartition)
        throw ToolError(what + "不是 PFS0");
    if (section.hashType() != NcaFsSection::HashSha256 && section.hashType() != NcaFsSection::HashSha3)
        throw ToolError(format("%s has a PFS0 with hash type %d, which isn't supported", what.c_str(), section.hashType()));

    const u8* h = section.header.data();
    int blockSize = rdi32(h + 0x28);
    i64 tableOffset = rdi64(h + 0x30);
    i64 tableSize = rdi64(h + 0x38);
    i64 dataOffset = rdi64(h + 0x40);
    i64 dataSize = rdi64(h + 0x48);
    if (blockSize <= 0 || tableOffset < 0 || tableSize < 0 || tableOffset + tableSize > logical->length() || dataOffset < 0 || dataSize < 0x10 || dataOffset + dataSize > logical->length())
        throw ToolError(what + "的 PFS0 布局无效");

    auto archive = std::make_unique<Pfs0Archive>();
    archive->_data = std::make_shared<SubSource>(logical, dataOffset, dataSize);
    archive->_hashBlockSize = blockSize;
    if (verify && section.hashType() == NcaFsSection::HashSha256) {
        Bytes table((size_t)tableSize);
        logical->read(tableOffset, table.data(), table.size());
        u8 hash[32];
        sha256(table.data(), table.size(), hash);
        if (memcmp(hash, h + 0x08, 32) != 0)
            throw ToolError(what + "：PFS0 主哈希校验失败，文件已损坏");

        // Unlike IVFC, a short last block in a PFS0 hash table is hashed at its real length.
        i64 blocks = (dataSize + blockSize - 1) / blockSize;
        if (blocks * 0x20 > (i64)table.size())
            throw ToolError(what + "：PFS0 哈希表过短，文件已损坏");
        Bytes buffer((size_t)blockSize);
        for (i64 i = 0; i < blocks; i++) {
            size_t n = (size_t)std::min<i64>(blockSize, dataSize - i * blockSize);
            archive->_data->read(i * blockSize, buffer.data(), n);
            sha256(buffer.data(), n, hash);
            if (memcmp(hash, table.data() + i * 0x20, 32) != 0)
                throw ToolError(format("%s: PFS0 hash check failed at offset 0x%llx, the file is damaged", what.c_str(), (long long)(i * blockSize)));
        }
    }
    archive->readEntries(what);
    return archive;
}

void Pfs0Archive::readEntries(const std::string& what) {
    u8 head[0x10];
    _data->read(0, head, sizeof(head));
    if (memcmp(head, "PFS0", 4) != 0)
        throw ToolError(what + "不以 PFS0 头开头");
    int count = rdi32(head + 4);
    int nameTable = rdi32(head + 8);
    i64 tableSize = count * 0x18LL + nameTable;
    if (count < 0 || nameTable < 0 || 0x10 + tableSize > _data->length())
        throw ToolError(what + "的 PFS0 头已损坏");

    Bytes table((size_t)tableSize);
    _data->read(0x10, table.data(), table.size());
    i64 dataStart = 0x10 + tableSize;
    const u8* names = table.data() + count * 0x18;
    for (int i = 0; i < count; i++) {
        const u8* e = table.data() + i * 0x18;
        i64 offset = rdi64(e);
        i64 size = rdi64(e + 8);
        int nameOffset = rdi32(e + 0x10);
        if (offset < 0 || size < 0 || dataStart + offset + size > _data->length() || nameOffset < 0 || nameOffset >= nameTable)
            throw ToolError(what + "的 PFS0 条目已损坏");
        const char* name = (const char*)names + nameOffset;
        std::string text(name, strnlen(name, (size_t)(nameTable - nameOffset)));
        LocalNames::checkEntryName(text, what);
        _files.push_back(Entry{text, dataStart + offset, size});
    }
}

namespace {

constexpr u32 RomFsEmpty = 0xFFFFFFFF;
constexpr i64 MaxRomFsTable = 1LL << 30;

const u8* tableSlice(const Bytes& table, i64 offset, i64 size, const std::string& what) {
    if (offset < 0 || size < 0 || offset + size > (i64)table.size())
        throw ToolError(what + "的 RomFS 表已损坏");
    return table.data() + offset;
}

u32 tableField(const Bytes& table, u32 entry, int field, const std::string& what) { return rd32(tableSlice(table, (i64)entry + field, 4, what)); }

std::string tableName(const Bytes& table, u32 entry, int sizeField, int nameField, const std::string& what) {
    u32 size = tableField(table, entry, sizeField, what);
    std::string name((const char*)tableSlice(table, (i64)entry + nameField, size, what), size);
    LocalNames::checkEntryName(name, what);
    return name;
}

Bytes readRomFsTable(const SourcePtr& image, i64 offset, i64 size, const std::string& what) {
    if (offset < 0 || size < 0 || size > MaxRomFsTable || offset + size > image->length())
        throw ToolError(what + "的 RomFS 头已损坏");
    Bytes table((size_t)size);
    image->read(offset, table.data(), table.size());
    return table;
}

}  // namespace

RomFsArchive RomFsArchive::open(const SourcePtr& image, const std::string& what) {
    u8 head[0x50];
    if (image->length() < (i64)sizeof(head))
        throw ToolError(what + "太小，无法容纳 RomFS");
    image->read(0, head, sizeof(head));
    auto field = [&](int i) { return rdi64(head + i * 8); };
    if (field(0) != 0x50)
        throw ToolError(what + "不以 RomFS 头开头");

    Bytes dirs = readRomFsTable(image, field(3), field(4), what);
    Bytes files = readRomFsTable(image, field(7), field(8), what);
    i64 partition = field(9);

    RomFsArchive archive;
    std::set<u32> seenDirs, seenFiles;
    std::vector<std::pair<u32, std::string>> pending;
    pending.emplace_back(0, "");
    while (!pending.empty()) {
        auto [dir, path] = pending.back();
        pending.pop_back();
        if (!seenDirs.insert(dir).second)
            throw ToolError(what + "的目录表中存在循环");

        for (u32 file = tableField(dirs, dir, 0x0C, what); file != RomFsEmpty; file = tableField(files, file, 0x04, what)) {
            if (!seenFiles.insert(file).second)
                throw ToolError(what + "的文件表中存在循环");
            std::string name = tableName(files, file, 0x1C, 0x20, what);
            i64 offset = rdi64(tableSlice(files, (i64)file + 0x08, 8, what));
            i64 size = rdi64(tableSlice(files, (i64)file + 0x10, 8, what));
            if (offset < 0 || size < 0 || partition + offset + size > image->length())
                throw ToolError(format("%s: file %s%s lies outside the RomFS", what.c_str(), path.c_str(), name.c_str()));
            archive.files.push_back(Entry{path + name, partition + offset, size});
        }

        for (u32 child = tableField(dirs, dir, 0x08, what); child != RomFsEmpty; child = tableField(dirs, child, 0x04, what)) {
            std::string childPath = path + tableName(dirs, child, 0x14, 0x18, what) + "/";
            archive.directories.push_back(childPath.substr(0, childPath.size() - 1));
            pending.emplace_back(child, childPath);
        }
    }
    return archive;
}

namespace LocalNames {

void checkEntryName(const std::string& name, const std::string& what) {
    bool unsafe = name.empty() || name == "." || name == "..";
    for (char c : name)
        unsafe = unsafe || c == '/' || c == '\\' || c == '\0';
    if (unsafe)
        throw ToolError(what + " contains an unsafe entry name: \"" + name + "\"");
}

static bool isBadCharacter(char c) { return (u8)c < 32 || c == '<' || c == '>' || c == ':' || c == '"' || c == '|' || c == '?' || c == '*'; }

std::string forFileSystem(const std::string& name) {
    std::string clean = name;
    for (char& c : clean) {
        if (isBadCharacter(c))
            c = '_';
    }
    while (!clean.empty() && (clean.back() == '.' || clean.back() == ' '))
        clean.back() = '_';
    return clean;
}

std::string forOutputName(const std::string& text) {
    std::string clean = text;
    for (char& c : clean) {
        if (isBadCharacter(c) || c == '/' || c == '\\')
            c = '_';
    }
    while (!clean.empty() && (clean.back() == ' ' || clean.back() == '.'))
        clean.pop_back();
    size_t start = 0;
    while (start < clean.size() && clean[start] == ' ')
        start++;
    return clean.substr(start);
}

}  // namespace LocalNames

}  // namespace hbp
