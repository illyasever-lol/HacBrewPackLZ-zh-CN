// An NCA stored as an NCZ, the way NSZ and XCZ files hold them, seen as the NCA it came from: the header as it was, followed
// by its sections, which here are already decrypted. There are two kinds. Block mode compresses each block on its own, so any
// part can be read at any time. Solid mode is one stream that can only be decoded from its start, so this works hard not to
// go back: small sections are kept in memory as they go past, the latest data can be read again, and if something keeps
// needing earlier data anyway, everything decoded from then on goes to a temporary file.
#pragma once

#include "zstd.h"

namespace hbp {

/// Reading on would mean decompressing a solid NCZ from its start again, which isn't allowed just now.
struct NczRewind {};

class NczSource : public DataSource {
public:
    NczSource(SourcePtr file, std::string name);
    ~NczSource() override;

    i64 length() const override { return _length; }
    void read(i64 offset, void* buffer, size_t count) override;

    bool isSolid() const { return _blockOffsets.empty(); }

    /// Says a section is small enough to hold on to. A solid stream keeps its bytes as they go past, so that the section can be
    /// read again, in any order, without starting the stream over.
    void keepIfSmall(i64 ncaOffset, i64 size);
    /// Lets go of the memory and any temporary file. Reading again later simply starts over.
    void releaseMemory();

    /// From now on this thread reads each solid stream in one pass: it can start over before its first read if it has to, and
    /// after that, a read of anything earlier throws NczRewind. This is for estimates, which must not end up decoding a whole
    /// game several times over.
    static void beginSinglePass();
    static void endSinglePass();

    /// Where a solid stream that can't be read in order is kept while it's in use. Set this before a job.
    static void setSpoolFolder(const std::string& folder);

private:
    struct KeptRange {
        i64 start;
        i64 size;
        std::unique_ptr<u8[]> data;
        i64 filled = 0;
    };

    [[noreturn]] void fail() const;
    int findSegment(i64 offset) const;
    void readBlocks(i64 position, u8* buffer, size_t count);
    int blockLength(int index) const;
    void loadBlock(int index);
    void readSolid(i64 position, u8* buffer, size_t count);
    size_t readKept(i64 position, u8* buffer, size_t count);
    void rewind(ZstdStream& stream);
    void onDecoded(i64 position, const u8* data, size_t count);

    SourcePtr _file;
    std::string _name;
    Bytes _header;
    int _headerEnd = 0;
    std::vector<i64> _ncaStarts;
    std::vector<i64> _sizes;
    std::vector<i64> _streamStarts;
    i64 _streamSize = 0;
    i64 _dataStart = 0;
    i64 _length = 0;

    std::vector<i64> _blockOffsets;
    std::vector<int> _blockSizes;
    int _blockExponent = 0;
    std::unique_ptr<ZstdDecoder> _blockDecoder;
    std::unique_ptr<u8[]> _block;
    std::unique_ptr<u8[]> _compressedBlock;
    int _blockIndex = -1;

    std::vector<std::unique_ptr<KeptRange>> _kept;
    std::unique_ptr<ZstdStream> _stream;
    std::unique_ptr<sys::File> _spool;
    std::string _spoolPath;
    i64 _spooled = 0;
    int _rewinds = 0;
    bool _warned = false;
    int _sweepSeen = 0;
};

}  // namespace hbp
