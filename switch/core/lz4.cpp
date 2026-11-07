#include "lz4.h"

#include <algorithm>
#include <climits>

namespace hbp {

namespace {

constexpr int MinMatch = 4;
constexpr int MfLimit = 12;
constexpr int LastLiterals = 5;
constexpr int HashBits = 16;

inline u32 hashOf(u32 sequence) { return (sequence * 2654435761u) >> (32 - HashBits); }

inline int commonLength(const u8* a, const u8* b, const u8* aEnd) {
    const u8* start = a;
    while (a + 8 <= aEnd) {
        u64 x = rd64(a) ^ rd64(b);
        if (x != 0)
            return (int)(a - start) + (lowestBit(x) >> 3);
        a += 8;
        b += 8;
    }
    while (a < aEnd && *a == *b) {
        a++;
        b++;
    }
    return (int)(a - start);
}

bool emitSequence(u8*& op, const u8* opEnd, const u8* literals, int literalCount, int distance, int matchLength) {
    if (op + 1 + literalCount / 255 + 1 + literalCount + 2 + matchLength / 255 + 1 > opEnd)
        return false;

    u8* token = op++;
    if (literalCount >= 15) {
        *token = 15 << 4;
        int r = literalCount - 15;
        for (; r >= 255; r -= 255)
            *op++ = 255;
        *op++ = (u8)r;
    } else {
        *token = (u8)(literalCount << 4);
    }

    memcpy(op, literals, (size_t)literalCount);
    op += literalCount;

    *op++ = (u8)distance;
    *op++ = (u8)(distance >> 8);

    int m = matchLength - MinMatch;
    if (m >= 15) {
        *token |= 15;
        m -= 15;
        for (; m >= 255; m -= 255)
            *op++ = 255;
        *op++ = (u8)m;
    } else {
        *token |= (u8)m;
    }
    return true;
}

bool emitLastLiterals(u8*& op, const u8* opEnd, const u8* literals, int count) {
    if (op + 1 + count / 255 + 1 + count > opEnd)
        return false;

    if (count >= 15) {
        *op++ = 15 << 4;
        int r = count - 15;
        for (; r >= 255; r -= 255)
            *op++ = 255;
        *op++ = (u8)r;
    } else {
        *op++ = (u8)(count << 4);
    }
    memcpy(op, literals, (size_t)count);
    op += count;
    return true;
}

/// One probe per position and no chains, skipping ahead faster the longer it goes without finding a match.
int compressGreedy(const u8* s, int n, u8* d, int capacity, u16* head, int skipShift) {
    u8* op = d;
    const u8* opEnd = d + capacity;
    const u8* limit = s + (n - LastLiterals);
    int ip = 0, anchor = 0;
    int mfLimit = n - MfLimit;

    while (ip <= mfLimit) {
        u32 sequence = rd32(s + ip);
        u32 h = hashOf(sequence);
        int candidate = head[h] - 1;
        head[h] = (u16)(ip + 1);
        if (candidate < 0 || rd32(s + candidate) != sequence) {
            ip += 1 + ((ip - anchor) >> skipShift);
            continue;
        }

        int length = MinMatch + commonLength(s + ip + MinMatch, s + candidate + MinMatch, limit);
        if (!emitSequence(op, opEnd, s + anchor, ip - anchor, ip - candidate, length))
            return -1;
        ip += length;
        anchor = ip;
    }
    return emitLastLiterals(op, opEnd, s + anchor, n - anchor) ? (int)(op - d) : -1;
}

int findLongest(const u8* s, u16* head, u16* prev, int& nextInsert, int ip, int matchEnd, int attempts, int& match) {
    // Positions are stored +1 so that 0 can mean "空". The last 12 bytes are never inserted, so they fit in 16 bits.
    for (int i = nextInsert; i < ip; i++) {
        u32 h = hashOf(rd32(s + i));
        prev[i] = head[h];
        head[h] = (u16)(i + 1);
    }
    nextInsert = ip;

    const u8* cur = s + ip;
    const u8* limit = s + matchEnd;
    u32 sequence = rd32(cur);
    int best = MinMatch - 1;
    match = -1;

    int candidate = head[hashOf(sequence)];
    while (candidate != 0 && attempts-- > 0) {
        int c = candidate - 1;
        if (s[c + best] == cur[best] && rd32(s + c) == sequence) {
            int length = MinMatch + commonLength(cur + MinMatch, s + c + MinMatch, limit);
            if (length > best) {
                best = length;
                match = c;
                if (cur + length >= limit)
                    break;
            }
        }
        candidate = prev[c];
    }
    return best >= MinMatch ? best : 0;
}

int compressLazy(const u8* s, int n, u8* d, int capacity, u16* head, u16* prev, int attempts) {
    constexpr int GoodEnough = 512;

    u8* op = d;
    const u8* opEnd = d + capacity;
    int ip = 0, anchor = 0, nextInsert = 0;
    int mfLimit = n - MfLimit, matchEnd = n - LastLiterals;

    while (ip <= mfLimit) {
        int match;
        int length = findLongest(s, head, prev, nextInsert, ip, matchEnd, attempts, match);
        if (length < MinMatch) {
            ip++;
            continue;
        }

        while (length < GoodEnough && ip + 1 <= mfLimit) {
            int nextMatch;
            int next = findLongest(s, head, prev, nextInsert, ip + 1, matchEnd, attempts, nextMatch);
            if (next > length) {
                ip++;
                length = next;
                match = nextMatch;
                continue;
            }
            if (ip + 2 > mfLimit)
                break;
            next = findLongest(s, head, prev, nextInsert, ip + 2, matchEnd, attempts, nextMatch);
            if (next > length + 1) {
                ip += 2;
                length = next;
                match = nextMatch;
                continue;
            }
            break;
        }

        if (!emitSequence(op, opEnd, s + anchor, ip - anchor, ip - match, length))
            return -1;
        ip += length;
        anchor = ip;
    }
    return emitLastLiterals(op, opEnd, s + anchor, n - anchor) ? (int)(op - d) : -1;
}

inline int matchLengthExtra(int length) {
    int m = length - MinMatch;
    return m < 15 ? 0 : 1 + (m - 15) / 255;
}

}  // namespace

Lz4Encoder::Lz4Encoder() : _head(new u16[1 << HashBits]), _prev(new u16[MaxBlockSize]) {}
Lz4Encoder::~Lz4Encoder() = default;

int Lz4Encoder::compress(const u8* src, int srcLength, u8* dst, int dstCapacity, int level) {
    if (srcLength > MaxBlockSize)
        throw ToolError("内部错误：LZ4 块过大");
    if (srcLength <= MfLimit)
        return -1;

    memset(_head.get(), 0, sizeof(u16) << HashBits);
    if (level <= 2)
        return compressGreedy(src, srcLength, dst, dstCapacity, _head.get(), level == 1 ? 5 : 8);
    if (level <= 9)
        return compressLazy(src, srcLength, dst, dstCapacity, _head.get(), _prev.get(), 1 << (level - 1));
    if (level == 10)
        return compressOptimal(src, srcLength, dst, dstCapacity, 96, 64);
    if (level == 11)
        return compressOptimal(src, srcLength, dst, dstCapacity, 512, 128);
    return compressOptimal(src, srcLength, dst, dstCapacity, 16384, 4096);
}

/// Forward dynamic programming over the exact byte cost of the LZ4 format. Offsets always cost two bytes, so at each
/// position only the longest match is needed: every shorter length can reuse its offset.
int Lz4Encoder::compressOptimal(const u8* s, int n, u8* d, int capacity, int attempts, int sufficient) {
    if (!_price) {
        _price.reset(new int[MaxBlockSize + 1]);
        _run.reset(new int[MaxBlockSize + 1]);
        _length.reset(new int[MaxBlockSize + 1]);
        _distance.reset(new u16[MaxBlockSize + 1]);
        _path.reset(new int[MaxBlockSize / MinMatch + 1]);
    }
    int* price = _price.get();
    int* run = _run.get();
    int* length = _length.get();
    u16* distance = _distance.get();
    int* path = _path.get();
    u16* head = _head.get();
    u16* prev = _prev.get();

    std::fill(price, price + n + 1, INT_MAX);
    price[0] = 0;
    run[0] = 0;
    length[0] = 0;

    int mfLimit = n - MfLimit, matchEnd = n - LastLiterals, nextInsert = 0;
    for (int cur = 0; cur < n; cur++) {
        int basePrice = price[cur];

        int literals = run[cur] + 1;
        int literalPrice = basePrice + 1 + (literals >= 15 && (literals - 15) % 255 == 0 ? 1 : 0);
        if (literalPrice < price[cur + 1]) {
            price[cur + 1] = literalPrice;
            run[cur + 1] = literals;
            length[cur + 1] = 0;
        }

        if (cur > mfLimit)
            continue;

        int match;
        int longest = findLongest(s, head, prev, nextInsert, cur, matchEnd, attempts, match);
        if (longest < MinMatch)
            continue;

        u16 dist = (u16)(cur - match);
        if (longest >= sufficient) {
            // Long enough that parsing inside it is not worth the time: this edge is the only way forward.
            int target = cur + longest;
            int matchPrice = basePrice + 3 + matchLengthExtra(longest);
            if (matchPrice < price[target]) {
                price[target] = matchPrice;
                run[target] = 0;
                length[target] = longest;
                distance[target] = dist;
            }
            cur = target - 1;
            continue;
        }

        for (int l = MinMatch; l <= longest; l++) {
            int target = cur + l;
            int matchPrice = basePrice + 3 + matchLengthExtra(l);
            if (matchPrice < price[target]) {
                price[target] = matchPrice;
                run[target] = 0;
                length[target] = l;
                distance[target] = dist;
            }
        }
    }

    int matches = 0;
    for (int pos = n; pos > 0;) {
        if (length[pos] == 0) {
            pos--;
        } else {
            path[matches++] = pos;
            pos -= length[pos];
        }
    }

    u8* op = d;
    const u8* opEnd = d + capacity;
    int anchor = 0;
    for (int i = matches - 1; i >= 0; i--) {
        int end = path[i];
        int start = end - length[end];
        if (!emitSequence(op, opEnd, s + anchor, start - anchor, distance[end], length[end]))
            return -1;
        anchor = end;
    }
    return emitLastLiterals(op, opEnd, s + anchor, n - anchor) ? (int)(op - d) : -1;
}

int lz4Decode(const u8* src, int srcLength, u8* dst, int dstCapacity) {
    int ip = 0, op = 0;
    while (ip < srcLength) {
        int token = src[ip++];

        int literals = token >> 4;
        if (literals == 15) {
            int b;
            do {
                if (ip >= srcLength)
                    return -1;
                b = src[ip++];
                literals += b;
            } while (b == 255);
        }
        if (literals > srcLength - ip || literals > dstCapacity - op)
            return -1;
        memcpy(dst + op, src + ip, (size_t)literals);
        ip += literals;
        op += literals;

        if (ip == srcLength)
            return op;

        if (srcLength - ip < 2)
            return -1;
        int distance = src[ip] | src[ip + 1] << 8;
        ip += 2;
        if (distance == 0 || distance > op)
            return -1;

        int length = token & 15;
        if (length == 15) {
            int b;
            do {
                if (ip >= srcLength)
                    return -1;
                b = src[ip++];
                length += b;
            } while (b == 255);
        }
        length += 4;
        if (length > dstCapacity - op)
            return -1;

        int from = op - distance;
        if (distance >= length) {
            memcpy(dst + op, dst + from, (size_t)length);
            op += length;
        } else {
            for (int i = 0; i < length; i++)
                dst[op++] = dst[from++];
        }
    }
    return -1;
}

}  // namespace hbp
