#include "zstd.h"

#include <algorithm>
#include <new>

namespace hbp {

namespace {

constexpr u32 FrameMagic = 0xFD2FB528;
constexpr u32 SkippableMagic = 0x184D2A50;
constexpr u32 SkippableMask = 0xFFFFFFF0;

constexpr int MaxHuffmanBits = 11;
constexpr int MaxWeightLog = 6;
constexpr int LiteralLengthMaxLog = 9;
constexpr int OffsetMaxLog = 8;
constexpr int MatchLengthMaxLog = 9;
constexpr int LiteralLengthMaxSymbol = 35;
constexpr int OffsetMaxSymbol = 31;
constexpr int MatchLengthMaxSymbol = 52;
constexpr int WildCopySize = 16;
constexpr int LookAhead = 8;
constexpr int CacheLine = 64;

struct Sequence {
    int literalLength;
    int matchLength;
    u64 offset;
};

struct FseEntry {
    u16 nextState;
    u8 symbol;
    u8 bits;
};

[[noreturn]] void corrupt() { throw ToolError("Zstandard 流已损坏，或不是 Zstandard 数据"); }

inline int log2Of(u32 value) { return highestBit(value); }

/// Reads a bitstream backwards from its end, the way Huffman and sequence streams are written.
struct BitReader {
    const u8* start;
    const u8* position;
    u64 bits = 0;
    u32 consumed;

    BitReader(const u8* source, int size) : start(source) {
        if (size < 1 || source[size - 1] == 0)
            corrupt();
        u32 marker = (u32)log2Of(source[size - 1]);
        if (size >= 8) {
            position = source + size - 8;
            bits = rd64(position);
            consumed = 8 - marker;
        } else {
            position = source;
            for (int i = 0; i < size; i++)
                bits |= (u64)source[i] << (8 * i);
            consumed = 8 - marker + (u32)(8 - size) * 8;
        }
    }

    bool isFinished() const { return position == start && consumed == 64; }

    u64 peek(int count) const { return bits << (consumed & 63) >> 1 >> ((63 - count) & 63); }
    void skip(int count) { consumed += (u32)count; }

    u64 read(int count) {
        u64 value = peek(count);
        consumed += (u32)count;
        return value;
    }

    /// Tops the container up to at least 56 unread bits, as long as there are that many left.
    void reload() {
        if (position - start >= 8) {
            position -= consumed >> 3;
            consumed &= 7;
            bits = rd64(position);
        } else if (position != start && consumed <= 64) {
            u32 bytes = std::min(consumed >> 3, (u32)(position - start));
            position -= bytes;
            consumed -= bytes * 8;
            bits = rd64(position);
        }
    }

    /// Decodes Huffman symbols five at a time until they, or the bits that can be loaded in one go, run low. Five symbols of
    /// at most 11 bits fit in what a load guarantees, whatever was left over from the last one.
    u8* decodeSymbols(u8* destination, const u8* end, const u16* table, int tableBits) {
        const u8* p = position;
        const u8* limit = start + 8;
        u32 used = consumed;
        int shift = 64 - tableBits;
        if (used >= 64)
            return destination;

        while (end - destination >= 5 && p >= limit) {
            p -= used >> 3;
            used &= 7;
            u64 value = rd64(p) << used;

            u32 entry = table[value >> shift];
            destination[0] = (u8)entry;
            value <<= entry >> 8;
            used += entry >> 8;

            entry = table[value >> shift];
            destination[1] = (u8)entry;
            value <<= entry >> 8;
            used += entry >> 8;

            entry = table[value >> shift];
            destination[2] = (u8)entry;
            value <<= entry >> 8;
            used += entry >> 8;

            entry = table[value >> shift];
            destination[3] = (u8)entry;
            value <<= entry >> 8;
            used += entry >> 8;

            entry = table[value >> shift];
            destination[4] = (u8)entry;
            used += entry >> 8;
            destination += 5;
        }

        position = p;
        consumed = used;
        bits = rd64(p);
        return destination;
    }
};

const u32 LiteralLengthBase[] = {0,  1,  2,  3,  4,  5,  6,  7,  8,   9,   10,  11,   12,   13,   14,   15,    16,    18,
                                 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
const u8 LiteralLengthExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const u32 MatchLengthBase[] = {3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13,  14,  15,  16,   17,   18,   19,   20,
                               21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,  32,  33,  34,   35,   37,   39,   41,
                               43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};
const u8 MatchLengthExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                               0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

const short DefaultLiteralCounts[] = {4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
const short DefaultOffsetCounts[] = {1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};
const short DefaultMatchCounts[] = {1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};

void buildTable(const short* counts, int symbols, int log, FseEntry* table, u16* nextStates) {
    int size = 1 << log;
    int mask = size - 1;
    int highThreshold = size;
    for (int s = 0; s < symbols; s++) {
        if (counts[s] == -1) {
            table[--highThreshold].symbol = (u8)s;
            nextStates[s] = 1;
        } else {
            nextStates[s] = (u16)std::max((int)counts[s], 0);
        }
    }

    int step = (size >> 1) + (size >> 3) + 3;
    int position = 0;
    for (int s = 0; s < symbols; s++) {
        for (int i = 0; i < counts[s]; i++) {
            table[position].symbol = (u8)s;
            do {
                position = (position + step) & mask;
            } while (position >= highThreshold);
        }
    }
    if (position != 0)
        corrupt();

    for (int i = 0; i < size; i++) {
        int state = nextStates[table[i].symbol]++;
        if (state <= 0)
            corrupt();
        int bits = log - log2Of((u32)state);
        table[i].bits = (u8)bits;
        table[i].nextState = (u16)((state << bits) - size);
    }
}

struct Defaults {
    FseEntry literal[1 << 6];
    FseEntry offset[1 << 5];
    FseEntry match[1 << 6];
    Defaults() {
        u16 next[256];
        buildTable(DefaultLiteralCounts, 36, 6, literal, next);
        buildTable(DefaultOffsetCounts, 29, 5, offset, next);
        buildTable(DefaultMatchCounts, 53, 6, match, next);
    }
};

const Defaults& defaults() {
    static const Defaults tables;
    return tables;
}

u32 readForward(const u8* source, int size, i64& position, int count) {
    u64 value = 0;
    i64 index = position >> 3;
    for (int i = 0; i < 5 && index + i < size; i++)
        value |= (u64)source[index + i] << (8 * i);
    u32 result = (u32)((value >> (position & 7)) & ((1ULL << count) - 1));
    position += count;
    return result;
}

int readBackward(const u8* source, i64& position, int count) {
    position -= count;
    i64 start = position;
    int bits = count;
    if (start < 0) {
        bits += (int)start;
        start = 0;
    }
    if (bits <= 0)
        return 0;

    int value = 0;
    for (int i = 0; i < bits; i++) {
        i64 bit = start + i;
        value |= ((source[bit >> 3] >> (bit & 7)) & 1) << i;
    }
    return position < 0 ? value << (int)-position : value;
}

inline void wildCopy(u8* destination, const u8* source, int count) {
    u8* end = destination + count;
    do {
        memcpy(destination, source, WildCopySize);
        destination += WildCopySize;
        source += WildCopySize;
    } while (destination < end);
}

inline void execute(const Sequence& sequence, u8*& output, const u8*& literal, const u8* literalEnd, const u8* history, const u8* outputEnd) {
    u8* op = output;
    int literalLength = sequence.literalLength;
    int matchLength = sequence.matchLength;
    u64 offset = sequence.offset;
    if (literalLength > literalEnd - literal || (i64)literalLength + matchLength > outputEnd - op)
        corrupt();

    if (outputEnd - op - literalLength >= WildCopySize)
        wildCopy(op, literal, literalLength);
    else
        memcpy(op, literal, (size_t)literalLength);
    op += literalLength;
    literal += literalLength;

    if (offset == 0 || offset > (u64)(op - history))
        corrupt();
    const u8* match = op - (i64)offset;
    if (offset >= WildCopySize && outputEnd - op - matchLength >= WildCopySize) {
        wildCopy(op, match, matchLength);
    } else if (offset == 1) {
        memset(op, *match, (size_t)matchLength);
    } else {
        for (int i = 0; i < matchLength; i++)
            op[i] = match[i];
    }
    output = op + matchLength;
}

}  // namespace

struct ZstdDecoder::Tables {
    u8 literals[MaxBlockSize + WildCopySize * 2];
    u16 huffman[1 << MaxHuffmanBits];
    FseEntry weightTable[1 << MaxWeightLog];
    FseEntry literalLengthTable[1 << LiteralLengthMaxLog];
    FseEntry offsetTable[1 << OffsetMaxLog];
    FseEntry matchLengthTable[1 << MatchLengthMaxLog];
    FseEntry literalLengthRle[1];
    FseEntry offsetRle[1];
    FseEntry matchLengthRle[1];
    short counts[256];
    u16 nextStates[256];

    const FseEntry* literalLengths = nullptr;
    const FseEntry* offsets = nullptr;
    const FseEntry* matchLengths = nullptr;
    int literalLengthLog = 0;
    int offsetLog = 0;
    int matchLengthLog = 0;
    int huffmanBits = 0;
    u64 repeat0 = 1;
    u64 repeat1 = 4;
    u64 repeat2 = 8;

    int readCounts(const u8* source, int size, int maxLog, int maxSymbol, int& symbols, int& log) {
        i64 position = 0;
        log = 5 + (int)readForward(source, size, position, 4);
        if (log > maxLog)
            corrupt();

        int remaining = 1 << log;
        int symbol = 0;
        while (remaining > 0 && symbol <= maxSymbol) {
            int bits = log2Of((u32)remaining + 1) + 1;
            int value = (int)readForward(source, size, position, bits);
            int lowerMask = (1 << (bits - 1)) - 1;
            int threshold = (1 << bits) - 1 - (remaining + 1);
            if ((value & lowerMask) < threshold) {
                position--;
                value &= lowerMask;
            } else if (value > lowerMask) {
                value -= threshold;
            }

            int count = value - 1;
            remaining -= count < 0 ? -count : count;
            counts[symbol++] = (short)count;

            if (count == 0) {
                int repeat = (int)readForward(source, size, position, 2);
                while (true) {
                    for (int i = 0; i < repeat; i++) {
                        if (symbol > maxSymbol)
                            corrupt();
                        counts[symbol++] = 0;
                    }
                    if (repeat != 3)
                        break;
                    repeat = (int)readForward(source, size, position, 2);
                }
            }
        }

        i64 bytes = (position + 7) >> 3;
        if (remaining != 0 || bytes > size)
            corrupt();
        symbols = symbol;
        return (int)bytes;
    }

    int readSequenceTable(int mode, const u8* source, int size, const FseEntry*& current, FseEntry* own, FseEntry* rle, const FseEntry* defaultTable, int defaultLog, int maxLog, int maxSymbol, int& log) {
        switch (mode) {
            case 0:
                current = defaultTable;
                log = defaultLog;
                return 0;
            case 1:
                if (size < 1 || source[0] > maxSymbol)
                    corrupt();
                rle[0] = FseEntry{0, source[0], 0};
                current = rle;
                log = 0;
                return 1;
            case 2: {
                int symbols;
                int used = readCounts(source, size, maxLog, maxSymbol, symbols, log);
                buildTable(counts, symbols, log, own, nextStates);
                current = own;
                return used;
            }
            default:
                if (current == nullptr)
                    corrupt();
                return 0;
        }
    }

    int readCompressedWeights(const u8* source, int size, u8* weights) {
        int symbols, log;
        int used = readCounts(source, size, MaxWeightLog, 255, symbols, log);
        buildTable(counts, symbols, log, weightTable, nextStates);
        source += used;
        size -= used;
        if (size < 1 || source[size - 1] == 0)
            corrupt();

        i64 position = size * 8LL - (8 - log2Of(source[size - 1]));
        int state1 = readBackward(source, position, log);
        int state2 = readBackward(source, position, log);
        int count = 0;
        while (true) {
            if (count >= 254)
                corrupt();
            FseEntry entry = weightTable[state1];
            weights[count++] = entry.symbol;
            state1 = entry.nextState + readBackward(source, position, entry.bits);
            if (position < 0) {
                weights[count++] = weightTable[state2].symbol;
                break;
            }

            entry = weightTable[state2];
            weights[count++] = entry.symbol;
            state2 = entry.nextState + readBackward(source, position, entry.bits);
            if (position < 0) {
                weights[count++] = weightTable[state1].symbol;
                break;
            }
        }
        return count;
    }

    int readHuffmanTable(const u8* source, int size) {
        if (size < 1)
            corrupt();

        u8 weights[256];
        int first = source[0];
        int count, used;
        if (first >= 128) {
            count = first - 127;
            used = 1 + (count + 1) / 2;
            if (used > size)
                corrupt();
            for (int i = 0; i < count; i++) {
                u8 packed = source[1 + i / 2];
                weights[i] = (u8)((i & 1) == 0 ? packed >> 4 : packed & 15);
            }
        } else {
            used = 1 + first;
            if (used > size)
                corrupt();
            count = readCompressedWeights(source + 1, first, weights);
        }

        u32 total = 0;
        for (int i = 0; i < count; i++) {
            if (weights[i] > MaxHuffmanBits)
                corrupt();
            if (weights[i] > 0)
                total += 1u << (weights[i] - 1);
        }
        if (total == 0)
            corrupt();

        int bits = log2Of(total) + 1;
        u32 left = (1u << bits) - total;
        if (bits > MaxHuffmanBits || (left & (left - 1)) != 0 || left == 0)
            corrupt();
        weights[count++] = (u8)(log2Of(left) + 1);

        int ranks[MaxHuffmanBits + 2] = {};
        for (int i = 0; i < count; i++) {
            if (weights[i] > 0)
                ranks[bits + 1 - weights[i]]++;
        }

        // The longest codes take the first table entries, then each shorter length follows in symbol order.
        int next[MaxHuffmanBits + 2] = {};
        next[bits] = 0;
        for (int i = bits; i >= 1; i--)
            next[i - 1] = next[i] + ranks[i] * (1 << (bits - i));

        for (int symbol = 0; symbol < count; symbol++) {
            if (weights[symbol] == 0)
                continue;
            int length = bits + 1 - weights[symbol];
            int span = 1 << (bits - length);
            int start = next[length];
            if (start + span > 1 << bits)
                corrupt();
            u16 entry = (u16)(symbol | length << 8);
            for (int i = 0; i < span; i++)
                huffman[start + i] = entry;
            next[length] = start + span;
        }
        huffmanBits = bits;
        return used;
    }

    void decodeHuffman(const u8* source, int size, u8* destination, int count) {
        BitReader reader(source, size);
        int bits = huffmanBits;
        const u8* end = destination + count;

        if (size >= 8)
            destination = reader.decodeSymbols(destination, end, huffman, bits);

        // What's left is the last few symbols, or the last few bytes of the stream, where fewer bits can be loaded.
        while (destination < end) {
            reader.reload();
            u16 entry = huffman[reader.peek(bits)];
            reader.skip(entry >> 8);
            *destination++ = (u8)entry;
        }

        reader.reload();
        if (!reader.isFinished())
            corrupt();
    }

    /// Reads the literals section. Returns the bytes it took up.
    int readLiterals(const u8* source, int size, int& literalCount) {
        if (size < 1)
            corrupt();

        int type = source[0] & 3;
        int format = (source[0] >> 2) & 3;
        int header, regenerated;
        if (type < 2) {
            switch (format) {
                case 1:
                    if (size < 2)
                        corrupt();
                    regenerated = source[0] >> 4 | source[1] << 4;
                    header = 2;
                    break;
                case 3:
                    if (size < 3)
                        corrupt();
                    regenerated = source[0] >> 4 | source[1] << 4 | source[2] << 12;
                    header = 3;
                    break;
                default:
                    regenerated = source[0] >> 3;
                    header = 1;
                    break;
            }
            if (regenerated > MaxBlockSize)
                corrupt();
            literalCount = regenerated;

            if (type == 0) {
                if (regenerated > size - header)
                    corrupt();
                memcpy(literals, source + header, (size_t)regenerated);
                return header + regenerated;
            }
            if (size - header < 1)
                corrupt();
            memset(literals, source[header], (size_t)regenerated);
            return header + 1;
        }

        int compressed;
        switch (format) {
            case 0:
            case 1: {
                if (size < 3)
                    corrupt();
                int small = source[0] | source[1] << 8 | source[2] << 16;
                regenerated = (small >> 4) & 0x3FF;
                compressed = (small >> 14) & 0x3FF;
                header = 3;
                break;
            }
            case 2: {
                if (size < 4)
                    corrupt();
                u32 medium = rd32(source);
                regenerated = (int)((medium >> 4) & 0x3FFF);
                compressed = (int)((medium >> 18) & 0x3FFF);
                header = 4;
                break;
            }
            default: {
                if (size < 5)
                    corrupt();
                u64 large = rd32(source) | (u64)source[4] << 32;
                regenerated = (int)((large >> 4) & 0x3FFFF);
                compressed = (int)((large >> 22) & 0x3FFFF);
                header = 5;
                break;
            }
        }
        if (regenerated > MaxBlockSize || compressed > size - header || compressed < 1)
            corrupt();
        literalCount = regenerated;

        const u8* stream = source + header;
        int left = compressed;
        if (type == 2) {
            int used = readHuffmanTable(stream, left);
            stream += used;
            left -= used;
        } else if (huffmanBits == 0) {
            corrupt();
        }

        if (format == 0) {
            decodeHuffman(stream, left, literals, regenerated);
        } else {
            if (left < 6)
                corrupt();
            int size1 = stream[0] | stream[1] << 8;
            int size2 = stream[2] | stream[3] << 8;
            int size3 = stream[4] | stream[5] << 8;
            stream += 6;
            left -= 6;
            int size4 = left - size1 - size2 - size3;
            int segment = (regenerated + 3) / 4;
            int lastSegment = regenerated - segment * 3;
            if (size4 < 1 || lastSegment < 0)
                corrupt();
            decodeHuffman(stream, size1, literals, segment);
            decodeHuffman(stream + size1, size2, literals + segment, segment);
            decodeHuffman(stream + size1 + size2, size3, literals + segment * 2, segment);
            decodeHuffman(stream + size1 + size2 + size3, size4, literals + segment * 3, lastSegment);
        }
        return header + compressed;
    }

    /// Strongly compressed games are full of short matches far back in a large window, and each of those would miss the CPU's
    /// cache. So sequences are decoded a little ahead of being carried out, and the memory each will read is fetched in the
    /// meantime, as the reference decoder does.
    int executeSequences(const u8* source, int size, int sequenceCount, int literalCount, const u8* history, u8* destination, const u8* destinationEnd) {
        BitReader reader(source, size);

        u8* op = destination;
        const u8* literal = literals;
        const u8* literalEnd = literals + literalCount;
        const u8* upcoming = destination;
        Sequence queue[LookAhead];
        u64 r0 = repeat0, r1 = repeat1, r2 = repeat2;

        reader.reload();
        int literalState = (int)reader.read(literalLengthLog);
        reader.reload();
        int offsetState = (int)reader.read(offsetLog);
        reader.reload();
        int matchState = (int)reader.read(matchLengthLog);

        for (int index = 0; index < sequenceCount; index++) {
            FseEntry literalEntry = literalLengths[literalState];
            FseEntry offsetEntry = offsets[offsetState];
            FseEntry matchEntry = matchLengths[matchState];

            // A reload always leaves at least 56 bits: enough for an offset and a match length, then for the rest.
            reader.reload();
            if (offsetEntry.symbol > 56)
                corrupt();
            u64 offset = (1ULL << offsetEntry.symbol) + reader.read(offsetEntry.symbol);
            int matchLength = (int)(MatchLengthBase[matchEntry.symbol] + (u32)reader.read(MatchLengthExtra[matchEntry.symbol]));
            reader.reload();
            int literalLength = (int)(LiteralLengthBase[literalEntry.symbol] + (u32)reader.read(LiteralLengthExtra[literalEntry.symbol]));

            if (offset > 3) {
                offset -= 3;
                r2 = r1;
                r1 = r0;
                r0 = offset;
            } else {
                switch ((int)offset - 1 + (literalLength == 0 ? 1 : 0)) {
                    case 0:
                        offset = r0;
                        break;
                    case 1:
                        offset = r1;
                        r1 = r0;
                        r0 = offset;
                        break;
                    case 2:
                        offset = r2;
                        r2 = r1;
                        r1 = r0;
                        r0 = offset;
                        break;
                    default:
                        offset = r0 - 1;
                        r2 = r1;
                        r1 = r0;
                        r0 = offset;
                        break;
                }
            }

            if (index + 1 < sequenceCount) {
                literalState = literalEntry.nextState + (int)reader.read(literalEntry.bits);
                matchState = matchEntry.nextState + (int)reader.read(matchEntry.bits);
                offsetState = offsetEntry.nextState + (int)reader.read(offsetEntry.bits);
            }

            // Asking for an address is harmless even if it turns out to be invalid: this reads nothing itself.
            upcoming += literalLength;
            const u8* from = (const u8*)((uintptr_t)upcoming - (uintptr_t)offset);
            prefetch(from);
            prefetch(from + CacheLine);
            upcoming += matchLength;

            if (index >= LookAhead)
                execute(queue[(index - LookAhead) & (LookAhead - 1)], op, literal, literalEnd, history, destinationEnd);
            queue[index & (LookAhead - 1)] = Sequence{literalLength, matchLength, offset};
        }

        for (int index = std::max(sequenceCount - LookAhead, 0); index < sequenceCount; index++)
            execute(queue[index & (LookAhead - 1)], op, literal, literalEnd, history, destinationEnd);

        reader.reload();
        if (!reader.isFinished())
            corrupt();
        repeat0 = r0;
        repeat1 = r1;
        repeat2 = r2;

        i64 left = literalEnd - literal;
        if (left > destinationEnd - op)
            corrupt();
        memcpy(op, literal, (size_t)left);
        op += left;
        return (int)(op - destination);
    }
};

ZstdDecoder::ZstdDecoder() : _t(new Tables) {}
ZstdDecoder::~ZstdDecoder() = default;

bool ZstdDecoder::readFrameHeader(const u8* source, size_t available, ZstdFrameHeader& header) {
    if (available < 5)
        return false;
    if (rd32(source) != FrameMagic)
        corrupt();

    u8 descriptor = source[4];
    int contentFlag = descriptor >> 6;
    bool singleSegment = (descriptor & 0x20) != 0;
    bool hasChecksum = (descriptor & 0x04) != 0;
    int dictionaryFlag = descriptor & 3;
    if ((descriptor & 0x08) != 0)
        corrupt();

    int dictionarySize = dictionaryFlag == 3 ? 4 : dictionaryFlag;
    int contentSize = contentFlag == 0 ? (singleSegment ? 1 : 0) : contentFlag == 1 ? 2 : contentFlag == 2 ? 4 : 8;
    size_t size = 5 + (singleSegment ? 0 : 1) + (size_t)dictionarySize + (size_t)contentSize;
    if (available < size)
        return false;

    size_t position = 5;
    i64 windowSize = 0;
    if (!singleSegment) {
        u8 window = source[position++];
        i64 windowBase = 1LL << (10 + (window >> 3));
        windowSize = windowBase + windowBase / 8 * (window & 7);
    }

    u64 dictionary = 0;
    for (int i = 0; i < dictionarySize; i++)
        dictionary |= (u64)source[position++] << (8 * i);
    if (dictionary != 0)
        throw ToolError("该 Zstandard 流需要字典，暂不支持");

    u64 content = 0;
    for (int i = 0; i < contentSize; i++)
        content |= (u64)source[position + (size_t)i] << (8 * i);
    if (contentFlag == 1)
        content += 256;
    if (content > (u64)INT64_MAX)
        corrupt();

    i64 known = contentSize > 0 ? (i64)content : -1;
    header.size = (int)size;
    header.windowSize = singleSegment ? known : windowSize;
    header.contentSize = known;
    header.hasChecksum = hasChecksum;
    return true;
}

i64 ZstdDecoder::skippableFrameSize(const u8* source, size_t size) {
    if (size < 8 || (rd32(source) & SkippableMask) != SkippableMagic)
        return -1;
    return 8LL + rd32(source + 4);
}

void ZstdDecoder::beginFrame() {
    _t->repeat0 = 1;
    _t->repeat1 = 4;
    _t->repeat2 = 8;
    _t->literalLengths = _t->offsets = _t->matchLengths = nullptr;
    _t->huffmanBits = 0;
}

size_t ZstdDecoder::decompress(const u8* source, size_t sourceSize, u8* destination, size_t capacity) {
    const u8* ip = source;
    const u8* iend = ip + sourceSize;
    u8* op = destination;
    const u8* oend = op + capacity;
    while (ip < iend) {
        size_t available = std::min<size_t>((size_t)(iend - ip), MaxFrameHeaderSize);
        i64 skip = skippableFrameSize(ip, available);
        if (skip >= 0) {
            if (skip > iend - ip)
                corrupt();
            ip += skip;
            continue;
        }

        ZstdFrameHeader header;
        if (!readFrameHeader(ip, available, header))
            corrupt();
        ip += header.size;
        beginFrame();
        const u8* history = op;

        bool last;
        do {
            if (iend - ip < 3)
                corrupt();
            int blockHeader = ip[0] | ip[1] << 8 | ip[2] << 16;
            ip += 3;
            last = (blockHeader & 1) != 0;
            int type = (blockHeader >> 1) & 3;
            int size = blockHeader >> 3;
            if (size > MaxBlockSize)
                corrupt();
            switch (type) {
                case 0:
                    if (size > iend - ip || size > oend - op)
                        corrupt();
                    memcpy(op, ip, (size_t)size);
                    ip += size;
                    op += size;
                    break;
                case 1:
                    if (iend - ip < 1 || size > oend - op)
                        corrupt();
                    memset(op, *ip, (size_t)size);
                    ip++;
                    op += size;
                    break;
                case 2:
                    if (size > iend - ip)
                        corrupt();
                    op += decodeBlock(ip, size, history, op, oend);
                    ip += size;
                    break;
                default:
                    corrupt();
            }
        } while (!last);

        if (header.hasChecksum) {
            if (iend - ip < 4)
                corrupt();
            ip += 4;
        }
    }
    return (size_t)(op - destination);
}

int ZstdDecoder::decodeBlock(const u8* source, int size, const u8* history, u8* destination, const u8* destinationEnd) {
    Tables& t = *_t;
    int literalCount;
    int consumed = t.readLiterals(source, size, literalCount);
    source += consumed;
    size -= consumed;
    if (size < 1)
        corrupt();

    int sequenceCount = source[0];
    int header = 1;
    if (sequenceCount >= 255) {
        if (size < 3)
            corrupt();
        sequenceCount = source[1] + (source[2] << 8) + 0x7F00;
        header = 3;
    } else if (sequenceCount >= 128) {
        if (size < 2)
            corrupt();
        sequenceCount = ((sequenceCount - 128) << 8) + source[1];
        header = 2;
    }
    source += header;
    size -= header;

    if (sequenceCount == 0) {
        if (size != 0 || literalCount > destinationEnd - destination)
            corrupt();
        memcpy(destination, t.literals, (size_t)literalCount);
        return literalCount;
    }

    if (size < 1)
        corrupt();
    int modes = *source++;
    size--;
    if ((modes & 3) != 0)
        corrupt();

    const Defaults& d = defaults();
    int used = t.readSequenceTable(modes >> 6, source, size, t.literalLengths, t.literalLengthTable, t.literalLengthRle, d.literal, 6, LiteralLengthMaxLog, LiteralLengthMaxSymbol, t.literalLengthLog);
    source += used;
    size -= used;
    used = t.readSequenceTable((modes >> 4) & 3, source, size, t.offsets, t.offsetTable, t.offsetRle, d.offset, 5, OffsetMaxLog, OffsetMaxSymbol, t.offsetLog);
    source += used;
    size -= used;
    used = t.readSequenceTable((modes >> 2) & 3, source, size, t.matchLengths, t.matchLengthTable, t.matchLengthRle, d.match, 6, MatchLengthMaxLog, MatchLengthMaxSymbol, t.matchLengthLog);
    source += used;
    size -= used;

    return t.executeSequences(source, size, sequenceCount, literalCount, history, destination, destinationEnd);
}

// -------------------------------------------------------------------------------------------------------------------

namespace {
constexpr size_t InputSize = 1 << 20;
constexpr i64 MinimumOutputSize = 1 << 24;
constexpr i64 MaxWindowSize = 1LL << 30;

[[noreturn]] void damaged() { throw ToolError("Zstandard 流已损坏，或提前结束"); }
}  // namespace

ZstdStream::ZstdStream(SourcePtr source, i64 start, i64 length, i64 expectedSize)
    : _source(std::move(source)), _start(start), _length(length), _expectedSize(expectedSize), _input(new u8[InputSize]) {}

void ZstdStream::rewind() {
    _inputPosition = _inputEnd = 0;
    _sourcePosition = 0;
    _outputEnd = 0;
    _frameStart = 0;
    _outputBase = 0;
    _inFrame = false;
}

size_t ZstdStream::copy(i64 position, u8* buffer, size_t count) {
    i64 inOutput = position - _outputBase;
    if (inOutput < 0 || inOutput >= (i64)_outputEnd)
        throw ToolError("内部错误：读取超出已解码内容范围");
    size_t n = std::min(count, (size_t)((i64)_outputEnd - inOutput));
    memcpy(buffer, _output.get() + inOutput, n);
    return n;
}

bool ZstdStream::advance() {
    checkStop();
    if (!_inFrame && !beginFrame())
        return false;

    require(3);
    int blockHeader = _input[_inputPosition] | _input[_inputPosition + 1] << 8 | _input[_inputPosition + 2] << 16;
    _inputPosition += 3;
    bool last = (blockHeader & 1) != 0;
    int type = (blockHeader >> 1) & 3;
    int size = blockHeader >> 3;
    if (size > ZstdDecoder::MaxBlockSize)
        damaged();

    makeRoom();
    int produced;
    switch (type) {
        case 0:
            require((size_t)size);
            memcpy(_output.get() + _outputEnd, _input.get() + _inputPosition, (size_t)size);
            _inputPosition += (size_t)size;
            produced = size;
            break;
        case 1:
            require(1);
            memset(_output.get() + _outputEnd, _input[_inputPosition], (size_t)size);
            _inputPosition++;
            produced = size;
            break;
        case 2:
            require((size_t)size);
            produced = _decoder.decodeBlock(_input.get() + _inputPosition, size, _output.get() + _frameStart, _output.get() + _outputEnd, _output.get() + _outputCapacity);
            _inputPosition += (size_t)size;
            break;
        default:
            damaged();
    }

    i64 before = position();
    _outputEnd += (size_t)produced;
    if (decoded)
        decoded(before, _output.get() + _outputEnd - produced, (size_t)produced);

    if (last) {
        if (_hasChecksum) {
            require(4);
            _inputPosition += 4;
        }
        _inFrame = false;
    }
    return true;
}

bool ZstdStream::beginFrame() {
    while (true) {
        fill(ZstdDecoder::MaxFrameHeaderSize);
        size_t available = _inputEnd - _inputPosition;
        if (available == 0)
            return false;

        const u8* head = _input.get() + _inputPosition;
        i64 skipSize = ZstdDecoder::skippableFrameSize(head, available);
        if (skipSize >= 0) {
            skip(skipSize);
            continue;
        }

        ZstdFrameHeader header;
        if (!ZstdDecoder::readFrameHeader(head, available, header))
            damaged();
        _inputPosition += (size_t)header.size;
        _hasChecksum = header.hasChecksum;
        _windowSize = std::max(std::min(header.windowSize, _expectedSize), (i64)ZstdDecoder::MaxBlockSize);
        if (_windowSize > MaxWindowSize)
            throw ToolError("该 Zstandard 流解码需要超过 1 GB 内存，暂不支持");
        growOutput(_windowSize);

        _decoder.beginFrame();
        makeRoom();
        _frameStart = _outputEnd;
        _inFrame = true;
        return true;
    }
}

void ZstdStream::growOutput(i64 windowSize) {
    i64 wanted = std::max(windowSize * 2, std::min(MinimumOutputSize, _expectedSize)) + ZstdDecoder::MaxBlockSize * 2;
    if ((i64)_outputCapacity >= wanted)
        return;
    u8* larger = new (std::nothrow) u8[(size_t)wanted];
    if (!larger)
        throw ToolError(format("There isn't enough free memory to decode this file: it needs %s", formatSize(wanted).c_str()));
    if (_outputEnd > 0)
        memcpy(larger, _output.get(), _outputEnd);
    _output.reset(larger);
    _outputCapacity = (size_t)wanted;
}

/// Keeps a window's worth of what was decoded, and moves it down once the rest of the buffer is used up.
void ZstdStream::makeRoom() {
    if (_outputEnd + ZstdDecoder::MaxBlockSize * 2 <= _outputCapacity)
        return;
    size_t keep = (size_t)std::min<i64>(_windowSize, (i64)_outputEnd);
    size_t removed = _outputEnd - keep;
    memmove(_output.get(), _output.get() + removed, keep);
    _outputEnd = keep;
    _outputBase += (i64)removed;
    _frameStart = _frameStart > removed ? _frameStart - removed : 0;
}

void ZstdStream::require(size_t count) {
    fill(count);
    if (_inputEnd - _inputPosition < count)
        damaged();
}

void ZstdStream::fill(size_t count) {
    if (_inputEnd - _inputPosition >= count)
        return;
    size_t left = _inputEnd - _inputPosition;
    memmove(_input.get(), _input.get() + _inputPosition, left);
    _inputPosition = 0;
    _inputEnd = left;
    size_t n = (size_t)std::min<i64>((i64)(InputSize - left), _length - _sourcePosition);
    if (n > 0) {
        _source->read(_start + _sourcePosition, _input.get() + left, n);
        _sourcePosition += (i64)n;
        _inputEnd += n;
    }
}

void ZstdStream::skip(i64 count) {
    i64 buffered = (i64)(_inputEnd - _inputPosition);
    if (count <= buffered) {
        _inputPosition += (size_t)count;
        return;
    }
    i64 beyond = count - buffered;
    if (beyond > _length - _sourcePosition)
        damaged();
    _sourcePosition += beyond;
    _inputPosition = _inputEnd = 0;
}

}  // namespace hbp
