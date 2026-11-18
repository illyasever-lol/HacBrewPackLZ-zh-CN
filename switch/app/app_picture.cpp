// Pictures inside games, for Look inside: PNG and BMP decoded here, the same on the console and in the tests, and the size of a JPEG, which
// the console's own decoder needs told.
#include "app_internal.h"

#include "../core/nacp.h"

namespace hbp {

namespace {

/// Big enough for a game's largest pictures, small enough to keep a decoded one in memory.
constexpr i64 MostPixels = 4096LL * 4096;

u32 rgba(u32 r, u32 g, u32 b, u32 a) { return r | g << 8 | b << 16 | a << 24; }

/// PNG keeps its numbers the other way round from the console.
u32 rd32be(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

}  // namespace

bool decodePng(const Bytes& data, Image& out) {
    static const u8 Signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (data.size() < 8 + 25 || memcmp(data.data(), Signature, 8) != 0)
        return false;
    u32 width = 0, height = 0;
    int depth = 0, colour = 0, interlace = 0;
    std::vector<u32> palette;
    Bytes packed;
    for (size_t at = 8; at + 12 <= data.size();) {
        u32 length = rd32be(data.data() + at);
        const u8* type = data.data() + at + 4;
        const u8* body = data.data() + at + 8;
        if (length > data.size() - at - 12)
            return false;
        if (memcmp(type, "IHDR", 4) == 0 && length >= 13) {
            width = rd32be(body);
            height = rd32be(body + 4);
            depth = body[8];
            colour = body[9];
            interlace = body[12];
        } else if (memcmp(type, "PLTE", 4) == 0) {
            for (u32 i = 0; i + 2 < length; i += 3)
                palette.push_back(rgba(body[i], body[i + 1], body[i + 2], 255));
        } else if (memcmp(type, "tRNS", 4) == 0 && colour == 3) {
            for (u32 i = 0; i < length && i < palette.size(); i++)
                palette[i] = (palette[i] & 0x00FFFFFFu) | (u32)body[i] << 24;
        } else if (memcmp(type, "IDAT", 4) == 0) {
            packed.insert(packed.end(), body, body + length);
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
        at += 12 + length;
    }
    // Interlaced pictures come in seven passes, which this doesn't put together: they're rare in games.
    if (width == 0 || height == 0 || (i64)width * height > MostPixels || interlace != 0 || packed.size() < 6)
        return false;
    int channels = colour == 0 ? 1 : colour == 2 ? 3 : colour == 3 ? 1 : colour == 4 ? 2 : colour == 6 ? 4 : 0;
    if (channels == 0 || !(depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16) || (colour == 3 && (depth == 16 || palette.empty())) ||
        ((colour == 2 || colour == 4 || colour == 6) && depth < 8))
        return false;
    size_t rowBytes = ((size_t)width * channels * depth + 7) / 8;
    size_t bpp = std::max<size_t>(1, (size_t)channels * depth / 8);
    Bytes rows((rowBytes + 1) * height);
    // A zlib stream: two bytes of header, then raw Deflate.
    if (inflateRaw(packed.data() + 2, packed.size() - 2, rows.data(), rows.size()) != (i64)rows.size())
        return false;

    out.w = (int)width;
    out.h = (int)height;
    out.pixels.assign((size_t)width * height, 0);
    Bytes previous(rowBytes, 0), line(rowBytes);
    for (u32 y = 0; y < height; y++) {
        const u8* in = rows.data() + (size_t)y * (rowBytes + 1);
        int filter = in[0];
        for (size_t x = 0; x < rowBytes; x++) {
            int a = x >= bpp ? line[x - bpp] : 0, b = previous[x], c = x >= bpp ? previous[x - bpp] : 0, value = in[1 + x];
            switch (filter) {
                case 0: break;
                case 1: value += a; break;
                case 2: value += b; break;
                case 3: value += (a + b) / 2; break;
                case 4: value += paeth(a, b, c); break;
                default: return false;
            }
            line[x] = (u8)value;
        }
        u32* pixel = out.pixels.data() + (size_t)y * width;
        auto sample = [&](u32 x, int channel) -> u32 {
            if (depth == 16)
                return line[(x * channels + channel) * 2];
            if (depth == 8)
                return line[x * channels + channel];
            // Below a byte, only one channel: grey or a place in the palette.
            u32 bit = x * depth;
            u32 raw = (line[bit / 8] >> (8 - depth - bit % 8)) & ((1u << depth) - 1);
            return colour == 3 ? raw : raw * 255 / ((1u << depth) - 1);
        };
        for (u32 x = 0; x < width; x++) {
            switch (colour) {
                case 0: { u32 g = sample(x, 0); pixel[x] = rgba(g, g, g, 255); break; }
                case 2: pixel[x] = rgba(sample(x, 0), sample(x, 1), sample(x, 2), 255); break;
                case 3: { u32 index = sample(x, 0); pixel[x] = index < palette.size() ? palette[index] : 0; break; }
                case 4: { u32 g = sample(x, 0); pixel[x] = rgba(g, g, g, sample(x, 1)); break; }
                default: pixel[x] = rgba(sample(x, 0), sample(x, 1), sample(x, 2), sample(x, 3)); break;
            }
        }
        std::swap(previous, line);
    }
    return true;
}

bool decodeBmp(const Bytes& data, Image& out) {
    if (data.size() < 54 || data[0] != 'B' || data[1] != 'M')
        return false;
    u32 start = rd32(data.data() + 10);
    i32 width = (i32)rd32(data.data() + 18), height = (i32)rd32(data.data() + 22);
    int bits = rd16(data.data() + 28);
    u32 compression = rd32(data.data() + 30);
    bool upward = height > 0;
    height = std::abs(height);
    // Plain 24 and 32 bits a pixel, which is what games keep, and the bitfields 32-bit ones are written with, in the usual order.
    if (width <= 0 || height <= 0 || (i64)width * height > MostPixels || !(bits == 24 || bits == 32) || !(compression == 0 || (compression == 3 && bits == 32)))
        return false;
    size_t stride = (((size_t)width * bits / 8) + 3) & ~(size_t)3;
    if (start > data.size() || data.size() - start < stride * (size_t)height)
        return false;
    out.w = width;
    out.h = height;
    out.pixels.resize((size_t)width * height);
    for (int y = 0; y < height; y++) {
        const u8* row = data.data() + start + stride * (size_t)(upward ? height - 1 - y : y);
        for (int x = 0; x < width; x++) {
            const u8* p = row + (size_t)x * bits / 8;
            out.pixels[(size_t)y * width + x] = rgba(p[2], p[1], p[0], bits == 32 && compression == 0 ? 255 : bits == 32 ? p[3] : 255);
        }
    }
    return true;
}

bool jpegSize(const Bytes& data, int& width, int& height) {
    if (data.size() < 4 || data[0] != 0xFF || data[1] != 0xD8)
        return false;
    for (size_t at = 2; at + 9 < data.size();) {
        if (data[at] != 0xFF) {
            at++;
            continue;
        }
        u8 marker = data[at + 1];
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            at += 2;
            continue;
        }
        size_t length = (size_t)data[at + 2] << 8 | data[at + 3];
        // Any start of frame but the ones that aren't images.
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            height = data[at + 5] << 8 | data[at + 6];
            width = data[at + 7] << 8 | data[at + 8];
            return width > 0 && height > 0;
        }
        at += 2 + length;
    }
    return false;
}

}  // namespace hbp
