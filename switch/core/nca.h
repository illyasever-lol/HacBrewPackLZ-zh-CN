// Reads an existing NCA in the same layer order as the firmware: decryption (with AES-CTR-Ex and the update's indirect table
// when there is one), then the hash tree, then decompression. Every block is checked against the hash tree on the way through,
// so damaged input or an update that doesn't belong to its base stops the job.
#pragma once

#include <map>
#include <set>

#include "crypto.h"
#include "ncz.h"
#include "source.h"

namespace hbp {

enum class NcaContentType : u8 { Program = 0, Meta = 1, Control = 2, Manual = 3, Data = 4, PublicData = 5 };

/// The BKTR tables of NCAs: a 16 KiB node tree over 16 KiB entry sets. Geometry matches fssystem::BucketTree.
namespace BucketTree {
constexpr int NodeSize = 0x4000;
constexpr int EntrySize = 0x18;
constexpr int NodeHeaderSize = 0x10;
constexpr int EntriesPerSet = (NodeSize - NodeHeaderSize) / EntrySize;
constexpr int OffsetsPerNode = (NodeSize - NodeHeaderSize) / 8;

int entrySetCount(int entries, int entrySize = EntrySize);
int l2NodeCount(int entries, int entrySize = EntrySize);
i64 nodeStorageSize(int entries, int entrySize = EntrySize);
i64 entryStorageSize(int entries, int entrySize = EntrySize);
int readHeader(const u8* header, const std::string& what);
/// Visits every entry of a table of any entry size, in order, and returns the table's end offset.
i64 enumerateEntries(const Bytes& table, int entrySize, int entryCount, const std::string& what, const std::function<void(int, const u8*)>& visit);
void writeHeader(u8* destination, int entries);

struct Entry {
    i64 virtualOffset;
    i64 physicalOffset;
    u8 type;
    int physicalSize;
};
/// Returns node storage followed by entry storage.
Bytes build(const std::vector<Entry>& entries, i64 virtualSize);
}  // namespace BucketTree

/// Title keys from the tickets of the files given, from the console, and from title.keys beside the keyset.
class TicketStore {
public:
    explicit TicketStore(const Keyset& keyset) : _keyset(keyset) {}
    void addTicket(const u8* ticket, size_t size);
    /// A key still wrapped with the title key encryption key, however it was come by.
    void addEncryptedKey(const std::string& rightsIdHex, const u8 key[16]);
    Bytes titleKey(const u8* rightsId, int generationIndex, const std::string& ncaName) const;

private:
    const Keyset& _keyset;
    std::map<std::string, Bytes> _encryptedKeys;
    std::set<std::string> _personalized;
};

struct NcaFsSection {
    static constexpr u8 FsRomFs = 0;
    static constexpr u8 FsPartition = 1;
    static constexpr u8 HashSha256 = 2;
    static constexpr u8 HashIntegrity = 3;
    static constexpr u8 HashSha3 = 5;
    static constexpr u8 HashIntegritySha3 = 6;
    static constexpr u8 EncryptNone = 1;
    static constexpr u8 EncryptCtr = 3;
    static constexpr u8 EncryptCtrEx = 4;

    int index = 0;
    i64 offset = 0;
    i64 size = 0;
    Bytes header;

    u8 fsType() const { return header[2]; }
    u8 hashType() const { return header[3]; }
    u8 encryption() const { return header[4]; }
    u64 upperIv() const { return rd64(&header[0x140]); }
    u32 secureValue() const { return rd32(&header[0x144]); }
    i64 indirectOffset() const { return rdi64(&header[0x100]); }
    i64 indirectSize() const { return rdi64(&header[0x108]); }
    const u8* indirectHeader() const { return &header[0x110]; }
    i64 ctrExOffset() const { return rdi64(&header[0x120]); }
    i64 ctrExSize() const { return rdi64(&header[0x128]); }
    const u8* ctrExHeader() const { return &header[0x130]; }
    bool hasIndirect() const { return indirectSize() != 0; }
    i64 sparseTableOffset() const { return rdi64(&header[0x148]); }
    i64 sparseTableSize() const { return rdi64(&header[0x150]); }
    const u8* sparseTableHeader() const { return &header[0x158]; }
    i64 sparseDataOffset() const { return rdi64(&header[0x168]); }
    u16 sparseGeneration() const { return rd16(&header[0x170]); }
    /// Most of it isn't in the file, as this base game was cut down to what its update still reads from it.
    bool hasSparse() const { return sparseGeneration() != 0; }
    i64 compressionOffset() const { return rdi64(&header[0x178]); }
    i64 compressionSize() const { return rdi64(&header[0x180]); }
    const u8* compressionHeader() const { return &header[0x188]; }
    bool hasCompression() const { return compressionOffset() != 0 && compressionSize() != 0; }
    /// What its hash tree covers, or nothing if it hasn't got that kind.
    i64 hashedBytes() const {
        int layers = rdi32(&header[0x14]);
        return memcmp(&header[0x08], "IVFC", 4) == 0 && layers >= 2 && layers <= 7 ? rdi64(&header[0x18 + (size_t)(layers - 2) * 0x18 + 8]) : 0;
    }
};

class Pfs0Archive;
class NcaFile;
using NcaPtr = std::shared_ptr<NcaFile>;

class NcaFile {
public:
    NcaFile(SourcePtr raw, std::string name, const Keyset& keyset, const TicketStore& tickets);

    const std::string& name() const { return _name; }
    const u8* header() const { return _header.data(); }
    const NcaFsSection* section(int index) const { return _sections[(size_t)index].get(); }
    static constexpr int SectionCount = 4;

    NcaContentType contentType() const { return (NcaContentType)_header[0x205]; }
    u64 programId() const { return rd64(&_header[0x210]); }
    int contentIndex() const { return rdi32(&_header[0x218]); }
    u32 sdkVersion() const { return rd32(&_header[0x21C]); }
    int generationIndex() const;
    bool hasRightsId() const;
    /// Whether any of it is only there in part, and needs an update to be whole.
    bool holdsOnlyPart() const;

    /// Data that can only be read efficiently from front to back, which is the case for a solid NCZ.
    bool readsInOrder() const { return _ncz && _ncz->isSolid(); }
    /// Lets go of what a compressed source holds on to. It's picked up again if the NCA is read again.
    void releaseMemory() { if (_ncz) _ncz->releaseMemory(); }

    /// The section's bytes after decryption, with no other layers applied.
    SourcePtr openPhysical(const NcaFsSection& section);
    /// The section as the hash tree sees it: an update's section is rebuilt from its base and its own data.
    SourcePtr openLogical(const NcaFsSection& section, NcaFile* baseNca);
    /// The plain RomFS image of a section, with decompression and normally hash checking applied. Verify is false only for
    /// estimates: hash blocks are large, so checking them defeats reading a small sample.
    SourcePtr openRomFsImage(const NcaFsSection& section, NcaFile* baseNca, bool verify = true);
    /// Up to the size given, it's only read the once, however many times its contents get gone through.
    std::unique_ptr<Pfs0Archive> openPfs0(const NcaFsSection& section, NcaFile* baseNca, bool verify = true, i64 keepInMemory = 0);

private:
    const Bytes& contentKey();
    Bytes readMetaTable(const NcaFsSection& section, i64 offset, i64 size);
    SourcePtr openSparse(const NcaFsSection& section);
    SourcePtr openIntegrityData(const NcaFsSection& section, SourcePtr logical, const std::string& what, const std::string& hint, bool wantVerify);
    SourcePtr openCompressed(const NcaFsSection& section, SourcePtr data);

    SourcePtr _raw;
    NczSource* _ncz;
    const Keyset& _keyset;
    const TicketStore& _tickets;
    Bytes _contentKey;
    std::string _name;
    Bytes _header;
    std::unique_ptr<NcaFsSection> _sections[SectionCount];
};

class Pfs0Archive {
public:
    struct Entry {
        std::string name;
        i64 offset;
        i64 size;
    };

    static std::unique_ptr<Pfs0Archive> open(SourcePtr logical, const NcaFsSection& section, const std::string& what, bool verify);

    const SourcePtr& data() const { return _data; }
    int hashBlockSize() const { return _hashBlockSize; }
    const std::vector<Entry>& files() const { return _files; }
    SourcePtr open(const Entry& entry) const { return std::make_shared<SubSource>(_data, entry.offset, entry.size); }
    Bytes readAll(const Entry& entry) const;

private:
    void readEntries(const std::string& what);
    SourcePtr _data;
    int _hashBlockSize = 0;
    std::vector<Entry> _files;
};

class RomFsArchive {
public:
    struct Entry {
        std::string path;
        i64 offset;
        i64 size;
    };

    static RomFsArchive open(const SourcePtr& image, const std::string& what);

    std::vector<std::string> directories;
    std::vector<Entry> files;
};

/// Names inside a title are untrusted: they must never be able to write outside the chosen folder.
namespace LocalNames {
void checkEntryName(const std::string& name, const std::string& what);
std::string forFileSystem(const std::string& name);
std::string forOutputName(const std::string& text);
}  // namespace LocalNames

}  // namespace hbp
