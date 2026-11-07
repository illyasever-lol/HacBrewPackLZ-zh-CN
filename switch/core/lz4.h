// LZ4 raw blocks of at most 64 KiB, which is all the NCA compressed storage layer accepts. The encoder makes exactly the same
// bytes as the C# one. Levels 1-2 are greedy, 3-9 lazy hash chain, 10-12 optimal parsing. Use one encoder per thread.
#pragma once

#include "common.h"

namespace hbp {

class Lz4Encoder {
public:
    static constexpr int MaxBlockSize = 0x10000;

    Lz4Encoder();
    ~Lz4Encoder();
    Lz4Encoder(const Lz4Encoder&) = delete;

    /// Returns the compressed size, or -1 when the result would not fit in the capacity.
    int compress(const u8* src, int srcLength, u8* dst, int dstCapacity, int level);

private:
    int compressOptimal(const u8* s, int n, u8* d, int capacity, int attempts, int sufficient);

    std::unique_ptr<u16[]> _head;
    std::unique_ptr<u16[]> _prev;
    std::unique_ptr<int[]> _price;
    std::unique_ptr<int[]> _run;
    std::unique_ptr<int[]> _length;
    std::unique_ptr<u16[]> _distance;
    std::unique_ptr<int[]> _path;
};

/// Strict decoder with full bounds checks. Returns the decoded size, or -1 if the input is malformed.
int lz4Decode(const u8* src, int srcLength, u8* dst, int dstCapacity);

}  // namespace hbp
