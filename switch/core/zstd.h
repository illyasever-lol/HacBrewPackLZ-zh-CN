// Zstandard decoding (RFC 8878), which is what NSZ and XCZ files are compressed with. Only decoding is needed, so this is all
// there is and no library comes along. Files can come from anywhere: every read and write is checked against its buffer, and
// anything malformed stops with a ToolError.
#pragma once

#include "source.h"

namespace hbp {

struct ZstdFrameHeader {
    int size = 0;
    i64 windowSize = 0;
    i64 contentSize = 0;
    bool hasChecksum = false;
};

class ZstdDecoder {
public:
    static constexpr int MaxBlockSize = 1 << 17;
    static constexpr int MaxFrameHeaderSize = 18;

    ZstdDecoder();
    ~ZstdDecoder();
    ZstdDecoder(const ZstdDecoder&) = delete;

    /// What's at the start of a frame, or false if more bytes are needed to tell. Not for skippable frames.
    static bool readFrameHeader(const u8* source, size_t size, ZstdFrameHeader& header);
    /// How far to skip for a skippable frame at the start of this data, or -1 if it isn't one.
    static i64 skippableFrameSize(const u8* source, size_t size);

    /// Forgets everything that carries over between blocks, as every new frame has to.
    void beginFrame();
    /// Decompresses whole frames held in memory, as in a block of an NSZ. Returns how many bytes they made.
    size_t decompress(const u8* source, size_t sourceSize, u8* destination, size_t capacity);
    /// Decodes one compressed block, with matches reaching back no further than history, and returns how many bytes it made.
    int decodeBlock(const u8* source, int size, const u8* history, u8* destination, const u8* destinationEnd);

private:
    struct Tables;
    std::unique_ptr<Tables> _t;
};

/// A Zstandard stream decoded front to back, as a solid NSZ has to be. What it decoded most recently stays in memory, because
/// later blocks refer back to it, and that much can also be read again without starting over.
class ZstdStream {
public:
    using DecodedFn = std::function<void(i64 position, const u8* data, size_t size)>;

    ZstdStream(SourcePtr source, i64 start, i64 length, i64 expectedSize);

    DecodedFn decoded;

    /// The stream position just after what has been decoded.
    i64 position() const { return _outputBase + _outputEnd; }
    /// The earliest stream position that can still be read without starting over.
    i64 oldest() const { return _outputBase; }

    void rewind();
    size_t copy(i64 position, u8* buffer, size_t count);
    /// Decodes the next block. False means the stream has ended.
    bool advance();

private:
    bool beginFrame();
    void growOutput(i64 windowSize);
    void makeRoom();
    void require(size_t count);
    void fill(size_t count);
    void skip(i64 count);

    SourcePtr _source;
    i64 _start;
    i64 _length;
    i64 _expectedSize;
    ZstdDecoder _decoder;
    std::unique_ptr<u8[]> _input;
    std::unique_ptr<u8[]> _output;
    size_t _outputCapacity = 0;
    size_t _inputPosition = 0;
    size_t _inputEnd = 0;
    i64 _sourcePosition = 0;
    size_t _outputEnd = 0;
    size_t _frameStart = 0;
    i64 _outputBase = 0;
    i64 _windowSize = 0;
    bool _inFrame = false;
    bool _hasChecksum = false;
};

}  // namespace hbp
