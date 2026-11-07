#include "nacp.h"

#include <algorithm>
#include <cstring>

namespace hbp {

// -------------------------------------------------------------------------------------------------------------------
// Deflate

namespace {

constexpr int MaxCodeLength = 15;
constexpr int LiteralCodes = 288;
constexpr int DistanceCodes = 30;

class BitReader {
public:
    BitReader(const u8* input, size_t size) : _input(input), _size(size) {}

    /// Negative once the input has run out.
    int bits(int count) {
        while (_count < count) {
            if (_position >= _size)
                return -1;
            _buffer |= (u32)_input[_position++] << _count;
            _count += 8;
        }
        int value = (int)(_buffer & ((1u << count) - 1));
        _buffer >>= count;
        _count -= count;
        return value;
    }

    void toByteBoundary() {
        _buffer = 0;
        _count = 0;
    }

    bool copy(u8* output, size_t count) {
        if (count > _size - _position)
            return false;
        memcpy(output, _input + _position, count);
        _position += count;
        return true;
    }

private:
    const u8* _input;
    size_t _size;
    size_t _position = 0;
    u32 _buffer = 0;
    int _count = 0;
};

struct HuffmanTable {
    short count[MaxCodeLength + 1];
    short symbol[LiteralCodes];

    /// False if the lengths ask for more codes than there can be.
    bool build(const u8* lengths, int symbols) {
        memset(count, 0, sizeof(count));
        for (int i = 0; i < symbols; i++)
            count[lengths[i]]++;
        int left = 1;
        for (int length = 1; length <= MaxCodeLength; length++) {
            left = left * 2 - count[length];
            if (left < 0)
                return false;
        }
        short offsets[MaxCodeLength + 2];
        offsets[1] = 0;
        for (int length = 1; length <= MaxCodeLength; length++)
            offsets[length + 1] = (short)(offsets[length] + count[length]);
        for (int i = 0; i < symbols; i++) {
            if (lengths[i] != 0)
                symbol[offsets[lengths[i]]++] = (short)i;
        }
        count[0] = 0;
        return true;
    }

    int decode(BitReader& reader) const {
        int code = 0, first = 0, index = 0;
        for (int length = 1; length <= MaxCodeLength; length++) {
            int bit = reader.bits(1);
            if (bit < 0)
                return -1;
            code |= bit;
            int here = count[length];
            if (code - here < first)
                return symbol[index + (code - first)];
            index += here;
            first = (first + here) * 2;
            code *= 2;
        }
        return -1;
    }
};

const short LengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const u8 LengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const short DistanceBase[DistanceCodes] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const u8 DistanceExtra[DistanceCodes] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

}  // namespace

i64 inflateRaw(const u8* input, size_t size, u8* output, size_t capacity) {
    BitReader reader(input, size);
    size_t written = 0;
    auto table = std::make_unique<HuffmanTable[]>(2);
    HuffmanTable& literals = table[0];
    HuffmanTable& distances = table[1];

    for (bool last = false; !last;) {
        int header = reader.bits(3);
        if (header < 0)
            return -1;
        last = (header & 1) != 0;
        int type = header >> 1;

        if (type == 0) {
            reader.toByteBoundary();
            u8 lengths[4];
            if (!reader.copy(lengths, 4))
                return -1;
            size_t length = (size_t)(lengths[0] | lengths[1] << 8);
            if ((lengths[2] | lengths[3] << 8) != (int)(~length & 0xFFFF))
                return -1;
            size_t fits = std::min(length, capacity - written);
            if (!reader.copy(output + written, fits))
                return -1;
            written += fits;
            if (fits < length)
                return (i64)written;
            continue;
        }
        if (type == 3)
            return -1;

        u8 lengths[LiteralCodes + DistanceCodes + 2];
        if (type == 1) {
            int i = 0;
            for (; i < 144; i++)
                lengths[i] = 8;
            for (; i < 256; i++)
                lengths[i] = 9;
            for (; i < 280; i++)
                lengths[i] = 7;
            for (; i < LiteralCodes; i++)
                lengths[i] = 8;
            literals.build(lengths, LiteralCodes);
            for (i = 0; i < DistanceCodes; i++)
                lengths[i] = 5;
            distances.build(lengths, DistanceCodes);
        } else {
            int literalCount = reader.bits(5);
            int distanceCount = reader.bits(5);
            int codeCount = reader.bits(4);
            if (literalCount < 0 || distanceCount < 0 || codeCount < 0)
                return -1;
            literalCount += 257;
            distanceCount += 1;
            codeCount += 4;
            if (literalCount > 286 || distanceCount > DistanceCodes)
                return -1;

            static const u8 order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            memset(lengths, 0, 19);
            for (int i = 0; i < codeCount; i++) {
                int length = reader.bits(3);
                if (length < 0)
                    return -1;
                lengths[order[i]] = (u8)length;
            }
            if (!literals.build(lengths, 19))
                return -1;

            int at = 0;
            while (at < literalCount + distanceCount) {
                int symbol = literals.decode(reader);
                if (symbol < 0)
                    return -1;
                if (symbol < 16) {
                    lengths[at++] = (u8)symbol;
                    continue;
                }
                int repeat, value = 0;
                if (symbol == 16) {
                    if (at == 0)
                        return -1;
                    value = lengths[at - 1];
                    repeat = reader.bits(2);
                    if (repeat < 0)
                        return -1;
                    repeat += 3;
                } else {
                    repeat = symbol == 17 ? reader.bits(3) : reader.bits(7);
                    if (repeat < 0)
                        return -1;
                    repeat += symbol == 17 ? 3 : 11;
                }
                if (at + repeat > literalCount + distanceCount)
                    return -1;
                memset(lengths + at, value, (size_t)repeat);
                at += repeat;
            }
            if (lengths[256] == 0)
                return -1;
            if (!literals.build(lengths, literalCount) || !distances.build(lengths + literalCount, distanceCount))
                return -1;
        }

        while (true) {
            int symbol = literals.decode(reader);
            if (symbol < 0)
                return -1;
            if (symbol == 256)
                break;
            if (symbol < 256) {
                if (written == capacity)
                    return (i64)written;
                output[written++] = (u8)symbol;
                continue;
            }
            symbol -= 257;
            if (symbol >= 29)
                return -1;
            int extra = reader.bits(LengthExtra[symbol]);
            if (extra < 0)
                return -1;
            size_t length = (size_t)(LengthBase[symbol] + extra);
            int code = distances.decode(reader);
            if (code < 0 || code >= DistanceCodes)
                return -1;
            extra = reader.bits(DistanceExtra[code]);
            if (extra < 0)
                return -1;
            size_t distance = (size_t)(DistanceBase[code] + extra);
            if (distance > written)
                return -1;
            for (size_t i = 0; i < length; i++) {
                if (written == capacity)
                    return (i64)written;
                output[written] = output[written - distance];
                written++;
            }
        }
    }
    return (i64)written;
}

// -------------------------------------------------------------------------------------------------------------------
// Names

namespace {

constexpr int PlainTitles = 16;
constexpr int MostTitles = 32;
constexpr size_t TitleEntrySize = 0x300;
constexpr size_t NameSize = 0x200;
constexpr size_t PublisherSize = 0x100;
constexpr size_t TitlesFormatOffset = 0x3215;
constexpr size_t CompressedCapacity = 0x2FFE;

bool isUtf8(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        u8 c = (u8)text[i];
        int more = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (more < 0 || i + (size_t)more >= text.size())
            return false;
        for (int k = 1; k <= more; k++) {
            if (((u8)text[i + (size_t)k] & 0xC0) != 0x80)
                return false;
        }
        i += (size_t)more + 1;
    }
    return true;
}

std::string field(const u8* data, size_t capacity) {
    const char* text = (const char*)data;
    size_t length = 0;
    while (length < capacity && text[length] != 0)
        length++;
    std::string value(text, length);
    return isUtf8(value) ? value : std::string();
}

std::vector<NacpTitle> entries(const u8* data, int count) {
    std::vector<NacpTitle> titles((size_t)count);
    for (int i = 0; i < count; i++) {
        const u8* entry = data + (size_t)i * TitleEntrySize;
        titles[(size_t)i].name = field(entry, NameSize);
        titles[(size_t)i].publisher = field(entry + NameSize, PublisherSize);
    }
    return titles;
}

}  // namespace

namespace Nacp {

std::vector<NacpTitle> titles(const u8* nacp, size_t size) {
    if (size < PlainTitles * TitleEntrySize)
        return {};
    int format = size > TitlesFormatOffset ? nacp[TitlesFormatOffset] : 0;
    if (format == 0)
        return entries(nacp, PlainTitles);
    if (format != 1)
        return {};

    size_t packed = (size_t)rd16(nacp);
    if (packed == 0 || packed > CompressedCapacity)
        return {};
    Bytes plain(MostTitles * TitleEntrySize);
    i64 length = inflateRaw(nacp + 2, packed, plain.data(), plain.size());
    if (length < 0)
        return {};
    return entries(plain.data(), (int)((size_t)length / TitleEntrySize));
}

NacpTitle choose(const std::vector<NacpTitle>& titles, int language) {
    if (language >= 0 && language < (int)titles.size() && !titles[(size_t)language].name.empty())
        return titles[(size_t)language];
    for (auto& title : titles) {
        if (!title.name.empty())
            return title;
    }
    return NacpTitle{};
}

std::string displayVersion(const u8* nacp, size_t size) {
    constexpr size_t At = 0x3060, Length = 0x10;
    if (size < At + Length)
        return "";
    const char* chars = (const char*)nacp + At;
    return std::string(chars, strnlen(chars, Length));
}

}  // namespace Nacp

}  // namespace hbp
