#include "canvas.h"

#include <algorithm>
#include <cmath>

namespace hbp {

Image Image::scaledTo(int width, int height) const {
    Image out;
    out.w = width;
    out.h = height;
    out.pixels.resize((size_t)width * height);
    if (empty())
        return out;
    for (int y = 0; y < height; y++) {
        int y0 = y * h / height, y1 = std::max((y + 1) * h / height, y0 + 1);
        for (int x = 0; x < width; x++) {
            int x0 = x * w / width, x1 = std::max((x + 1) * w / width, x0 + 1);
            u32 sum[4] = {};
            for (int sy = y0; sy < y1; sy++) {
                for (int sx = x0; sx < x1; sx++) {
                    u32 p = pixels[(size_t)sy * w + sx];
                    sum[0] += p & 255;
                    sum[1] += p >> 8 & 255;
                    sum[2] += p >> 16 & 255;
                    sum[3] += p >> 24 & 255;
                }
            }
            u32 n = (u32)((y1 - y0) * (x1 - x0));
            out.pixels[(size_t)y * width + x] = (sum[0] / n) | (sum[1] / n) << 8 | (sum[2] / n) << 16 | (sum[3] / n) << 24;
        }
    }
    return out;
}

// -------------------------------------------------------------------------------------------------------------------
// TrueType

namespace {

constexpr int CompositeWords = 0x0001;
constexpr int CompositeXyValues = 0x0002;
constexpr int CompositeScale = 0x0008;
constexpr int CompositeMore = 0x0020;
constexpr int CompositeXyScale = 0x0040;
constexpr int CompositeMatrix = 0x0080;

inline u16 be16(const u8* p) { return (u16)(p[0] << 8 | p[1]); }
inline short bes16(const u8* p) { return (short)be16(p); }
inline u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

struct Point {
    float x, y;
    bool on;
};

/// Adds a line's signed area to a row of cells: summed from left to right afterwards, that gives exact coverage.
void addLine(float* a, int w, int h, float x0, float y0, float x1, float y1) {
    if (y0 == y1)
        return;
    float dir = 1;
    if (y0 > y1) {
        std::swap(x0, x1);
        std::swap(y0, y1);
        dir = -1;
    }
    float dxdy = (x1 - x0) / (y1 - y0);
    float x = x0;
    if (y0 < 0)
        x -= y0 * dxdy;
    int stride = w + 2;
    for (int y = std::max(0, (int)floorf(y0)); y < std::min(h, (int)ceilf(y1)); y++) {
        float* row = a + y * stride;
        float dy = std::min((float)(y + 1), y1) - std::max((float)y, y0);
        float xnext = x + dxdy * dy;
        float d = dy * dir;
        float xa = std::min(x, xnext), xb = std::max(x, xnext);
        xa = std::max(xa, 0.0f);
        xb = std::min(xb, (float)w);
        float xaFloor = floorf(xa);
        int xai = (int)xaFloor;
        float xbCeil = ceilf(xb);
        int xbi = (int)xbCeil;
        if (xbi <= xai + 1) {
            float xmf = 0.5f * (xa + xb) - xaFloor;
            row[xai] += d - d * xmf;
            row[xai + 1] += d * xmf;
        } else {
            float s = 1.0f / (xb - xa);
            float xaf = xa - xaFloor;
            float a0 = 0.5f * s * (1 - xaf) * (1 - xaf);
            float xbf = xb - xbCeil + 1;
            float am = 0.5f * s * xbf * xbf;
            row[xai] += d * a0;
            if (xbi == xai + 2) {
                row[xai + 1] += d * (1 - a0 - am);
            } else {
                float a1 = s * (1.5f - xaf);
                row[xai + 1] += d * (a1 - a0);
                for (int xi = xai + 2; xi < xbi - 1; xi++)
                    row[xi] += d * s;
                float a2 = a1 + (xbi - xai - 3) * s;
                row[xbi - 1] += d * (1 - a2 - am);
            }
            row[xbi] += d * am;
        }
        x = xnext;
    }
}

void addQuad(float* a, int w, int h, Point p0, Point p1, Point p2) {
    float dx = p0.x - 2 * p1.x + p2.x, dy = p0.y - 2 * p1.y + p2.y;
    int steps = std::max(1, std::min(16, (int)(sqrtf(sqrtf(dx * dx + dy * dy)) * 2.5f) + 1));
    float px = p0.x, py = p0.y;
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / steps;
        float mt = 1 - t;
        float x = mt * mt * p0.x + 2 * mt * t * p1.x + t * t * p2.x;
        float y = mt * mt * p0.y + 2 * mt * t * p1.y + t * t * p2.y;
        addLine(a, w, h, px, py, x, y);
        px = x;
        py = y;
    }
}

/// One of the lists a CFF table is made of.
struct CffIndex {
    u32 count = 0;
    int offsetSize = 0;
    const u8* offsets = nullptr;
    const u8* objects = nullptr;
    const u8* end = nullptr;

    bool read(const u8* p, const u8* limit) {
        if (p + 2 > limit)
            return false;
        count = be16(p);
        if (count == 0) {
            end = p + 2;
            return true;
        }
        offsetSize = p[2];
        if (offsetSize < 1 || offsetSize > 4)
            return false;
        offsets = p + 3;
        objects = offsets + (count + 1) * (size_t)offsetSize - 1;
        if (objects > limit)
            return false;
        u32 last = offsetAt(count);
        end = objects + last;
        return end <= limit;
    }

    u32 offsetAt(u32 i) const {
        u32 value = 0;
        for (int b = 0; b < offsetSize; b++)
            value = value << 8 | offsets[i * offsetSize + b];
        return value;
    }

    bool get(u32 i, const u8*& begin, const u8*& finish) const {
        if (i >= count)
            return false;
        begin = objects + offsetAt(i);
        finish = objects + offsetAt(i + 1);
        return begin <= finish && finish <= end;
    }
};

/// Goes through a CFF dictionary, handing over each operator with its operands.
template <typename Fn>
void readCffDict(const u8* p, const u8* end, Fn fn) {
    double stack[48];
    int n = 0;
    while (p < end) {
        int b = *p++;
        if (b <= 21) {
            int op = b;
            if (b == 12 && p < end)
                op = 1200 + *p++;
            fn(op, stack, n);
            n = 0;
            continue;
        }
        double value = 0;
        if (b == 28 && p + 2 <= end) {
            value = bes16(p);
            p += 2;
        } else if (b == 29 && p + 4 <= end) {
            value = (i32)be32(p);
            p += 4;
        } else if (b == 30) {
            // A real number, a nibble at a time. Only its place in the list matters here.
            while (p < end) {
                int nibbles = *p++;
                if ((nibbles & 15) == 15 || nibbles >> 4 == 15)
                    break;
            }
        } else if (b >= 32 && b <= 246) {
            value = b - 139;
        } else if (b >= 247 && b <= 250 && p < end) {
            value = (b - 247) * 256 + *p++ + 108;
        } else if (b >= 251 && b <= 254 && p < end) {
            value = -(b - 251) * 256 - *p++ - 108;
        }
        if (n < 48)
            stack[n++] = value;
    }
}

}  // namespace

struct Font::Face {
    const u8* data = nullptr;
    size_t dataSize = 0;
    const u8* head = nullptr;
    const u8* hhea = nullptr;
    const u8* hmtx = nullptr;
    const u8* loca = nullptr;
    const u8* glyf = nullptr;
    const u8* cmap = nullptr;
    u32 glyfSize = 0, locaSize = 0, hmtxSize = 0;
    CffIndex charStrings, globalSubrs;
    std::vector<CffIndex> localSubrs;
    const u8* fdSelect = nullptr;
    int unitsPerEm = 1000;
    int numHMetrics = 0;
    bool longLoca = false;

    bool load() {
        if (dataSize < 12)
            return false;
        u32 tag = be32(data);
        size_t offset = 0;
        if (tag == 0x74746366)  // A collection: the first font in it.
            offset = be32(data + 12);
        if (offset + 12 > dataSize)
            return false;
        int tables = be16(data + offset + 4);
        const u8* maxp = nullptr;
        for (int i = 0; i < tables; i++) {
            const u8* entry = data + offset + 12 + i * 16;
            if (entry + 16 > data + dataSize)
                return false;
            u32 at = be32(entry + 8), size = be32(entry + 12);
            if ((u64)at + size > dataSize)
                continue;
            const u8* table = data + at;
            if (!memcmp(entry, "head", 4))
                head = table;
            else if (!memcmp(entry, "hhea", 4))
                hhea = table;
            else if (!memcmp(entry, "hmtx", 4)) {
                hmtx = table;
                hmtxSize = size;
            } else if (!memcmp(entry, "loca", 4)) {
                loca = table;
                locaSize = size;
            } else if (!memcmp(entry, "glyf", 4)) {
                glyf = table;
                glyfSize = size;
            } else if (!memcmp(entry, "cmap", 4))
                cmap = table;
            else if (!memcmp(entry, "maxp", 4))
                maxp = table;
            else if (!memcmp(entry, "CFF ", 4) && !loadCff(table, size))
                return false;
        }
        if (!head || !hhea || !hmtx || !cmap || !maxp)
            return false;
        if ((!loca || !glyf) && charStrings.count == 0)
            return false;
        unitsPerEm = std::max<int>(be16(head + 18), 1);
        longLoca = be16(head + 50) != 0;
        numHMetrics = be16(hhea + 34);
        return true;
    }

    bool loadCff(const u8* table, u32 size) {
        const u8* limit = table + size;
        if (size < 4)
            return false;
        CffIndex names, topDicts, strings;
        if (!names.read(table + table[2], limit) || !topDicts.read(names.end, limit) || !strings.read(topDicts.end, limit) || !globalSubrs.read(strings.end, limit))
            return false;
        const u8* begin;
        const u8* end;
        if (!topDicts.get(0, begin, end))
            return false;

        i64 charStringsAt = 0, fdArrayAt = 0, fdSelectAt = 0, privateSize = 0, privateAt = 0;
        readCffDict(begin, end, [&](int op, const double* s, int n) {
            if (op == 17 && n >= 1)
                charStringsAt = (i64)s[0];
            else if (op == 18 && n >= 2) {
                privateSize = (i64)s[0];
                privateAt = (i64)s[1];
            } else if (op == 1236 && n >= 1)
                fdArrayAt = (i64)s[0];
            else if (op == 1237 && n >= 1)
                fdSelectAt = (i64)s[0];
        });
        if (charStringsAt <= 0 || charStringsAt >= size || !charStrings.read(table + charStringsAt, limit))
            return false;

        auto readPrivate = [&](i64 length, i64 at) {
            CffIndex subrs;
            if (at <= 0 || at + length > size)
                return subrs;
            i64 subrsAt = 0;
            readCffDict(table + at, table + at + length, [&](int op, const double* s, int n) {
                if (op == 19 && n >= 1)
                    subrsAt = (i64)s[0];
            });
            if (subrsAt > 0 && at + subrsAt < size && !subrs.read(table + at + subrsAt, limit))
                subrs = CffIndex();
            return subrs;
        };

        if (fdArrayAt > 0 && fdArrayAt < size && fdSelectAt > 0 && fdSelectAt < size) {
            // A font made for a lot of characters keeps them in groups, each with its own subroutines.
            CffIndex fonts;
            if (!fonts.read(table + fdArrayAt, limit))
                return false;
            for (u32 i = 0; i < fonts.count; i++) {
                i64 length = 0, at = 0;
                if (fonts.get(i, begin, end)) {
                    readCffDict(begin, end, [&](int op, const double* s, int n) {
                        if (op == 18 && n >= 2) {
                            length = (i64)s[0];
                            at = (i64)s[1];
                        }
                    });
                }
                localSubrs.push_back(readPrivate(length, at));
            }
            fdSelect = table + fdSelectAt;
        } else {
            localSubrs.push_back(readPrivate(privateSize, privateAt));
        }
        return true;
    }

    int groupOf(int glyph) const {
        if (!fdSelect)
            return 0;
        if (fdSelect[0] == 0)
            return fdSelect[1 + glyph];
        if (fdSelect[0] == 3) {
            int ranges = be16(fdSelect + 1);
            const u8* range = fdSelect + 3;
            for (int i = 0; i < ranges; i++, range += 3) {
                if (glyph >= be16(range) && glyph < be16(range + 3))
                    return range[2];
            }
        }
        return 0;
    }

    /// Follows the drawing commands of a glyph, whose curves come out as a run of short lines.
    struct CffPen {
        const Face& face;
        const CffIndex& local;
        float scale;
        std::vector<std::vector<Point>>& out;
        std::vector<Point> contour;
        float stack[64];
        int n = 0;
        int stems = 0;
        bool widthSeen = false;
        bool done = false;
        float x = 0, y = 0;

        void plot(float px, float py) { contour.push_back(Point{px * scale, py * scale, true}); }

        void moveTo(float dx, float dy) {
            finish();
            x += dx;
            y += dy;
            plot(x, y);
        }

        void lineTo(float dx, float dy) {
            x += dx;
            y += dy;
            plot(x, y);
        }

        void curveTo(float dx1, float dy1, float dx2, float dy2, float dx3, float dy3) {
            float x0 = x, y0 = y;
            float x1 = x0 + dx1, y1 = y0 + dy1;
            float x2 = x1 + dx2, y2 = y1 + dy2;
            float x3 = x2 + dx3, y3 = y2 + dy3;
            float length = (fabsf(dx1) + fabsf(dy1) + fabsf(dx2) + fabsf(dy2) + fabsf(dx3) + fabsf(dy3)) * scale;
            int steps = std::max(2, std::min(12, (int)(length / 2.5f)));
            for (int i = 1; i <= steps; i++) {
                float t = (float)i / steps, mt = 1 - t;
                float a = mt * mt * mt, b = 3 * mt * mt * t, c = 3 * mt * t * t, d = t * t * t;
                plot(a * x0 + b * x1 + c * x2 + d * x3, a * y0 + b * y1 + c * y2 + d * y3);
            }
            x = x3;
            y = y3;
        }

        void finish() {
            if (contour.size() > 1)
                out.push_back(contour);
            contour.clear();
        }

        /// The first command may start with the glyph's width, which is known from elsewhere.
        void skipWidth(int expected) {
            if (!widthSeen && n > expected) {
                for (int i = 1; i < n; i++)
                    stack[i - 1] = stack[i];
                n--;
            }
            widthSeen = true;
        }

        static int bias(u32 count) { return count < 1240 ? 107 : count < 33900 ? 1131 : 32768; }

        void run(const u8* p, const u8* end, int depth) {
            if (depth > 10)
                return;
            while (p < end && !done) {
                int b = *p++;
                if (b >= 32 || b == 28) {
                    float value = 0;
                    if (b == 28 && p + 2 <= end) {
                        value = bes16(p);
                        p += 2;
                    } else if (b >= 32 && b <= 246) {
                        value = (float)(b - 139);
                    } else if (b >= 247 && b <= 250 && p < end) {
                        value = (float)((b - 247) * 256 + *p++ + 108);
                    } else if (b >= 251 && b <= 254 && p < end) {
                        value = (float)(-(b - 251) * 256 - *p++ - 108);
                    } else if (b == 255 && p + 4 <= end) {
                        value = (float)(i32)be32(p) / 65536.0f;
                        p += 4;
                    }
                    if (n < 64)
                        stack[n++] = value;
                    continue;
                }

                switch (b) {
                    case 1:
                    case 3:
                    case 18:
                    case 23:
                        // Stem hints, which only have to be counted.
                        skipWidth(n & ~1);
                        stems += n / 2;
                        n = 0;
                        break;
                    case 19:
                    case 20:
                        skipWidth(n & ~1);
                        stems += n / 2;
                        n = 0;
                        p += (stems + 7) / 8;
                        break;
                    case 21:
                        skipWidth(2);
                        if (n >= 2)
                            moveTo(stack[0], stack[1]);
                        n = 0;
                        break;
                    case 22:
                        skipWidth(1);
                        if (n >= 1)
                            moveTo(stack[0], 0);
                        n = 0;
                        break;
                    case 4:
                        skipWidth(1);
                        if (n >= 1)
                            moveTo(0, stack[0]);
                        n = 0;
                        break;
                    case 5:
                        for (int i = 0; i + 1 < n; i += 2)
                            lineTo(stack[i], stack[i + 1]);
                        n = 0;
                        break;
                    case 6:
                    case 7: {
                        bool horizontal = b == 6;
                        for (int i = 0; i < n; i++, horizontal = !horizontal)
                            lineTo(horizontal ? stack[i] : 0, horizontal ? 0 : stack[i]);
                        n = 0;
                        break;
                    }
                    case 8:
                        for (int i = 0; i + 5 < n; i += 6)
                            curveTo(stack[i], stack[i + 1], stack[i + 2], stack[i + 3], stack[i + 4], stack[i + 5]);
                        n = 0;
                        break;
                    case 24: {
                        int i = 0;
                        for (; i + 7 < n; i += 6)
                            curveTo(stack[i], stack[i + 1], stack[i + 2], stack[i + 3], stack[i + 4], stack[i + 5]);
                        if (i + 1 < n)
                            lineTo(stack[i], stack[i + 1]);
                        n = 0;
                        break;
                    }
                    case 25: {
                        int i = 0;
                        for (; i + 7 < n; i += 2)
                            lineTo(stack[i], stack[i + 1]);
                        if (i + 5 < n)
                            curveTo(stack[i], stack[i + 1], stack[i + 2], stack[i + 3], stack[i + 4], stack[i + 5]);
                        n = 0;
                        break;
                    }
                    case 26: {
                        int i = 0;
                        float dx = 0;
                        if (n & 1)
                            dx = stack[i++];
                        for (; i + 3 < n; i += 4) {
                            curveTo(dx, stack[i], stack[i + 1], stack[i + 2], 0, stack[i + 3]);
                            dx = 0;
                        }
                        n = 0;
                        break;
                    }
                    case 27: {
                        int i = 0;
                        float dy = 0;
                        if (n & 1)
                            dy = stack[i++];
                        for (; i + 3 < n; i += 4) {
                            curveTo(stack[i], dy, stack[i + 1], stack[i + 2], stack[i + 3], 0);
                            dy = 0;
                        }
                        n = 0;
                        break;
                    }
                    case 30:
                    case 31: {
                        bool horizontal = b == 31;
                        int i = 0;
                        while (i + 3 < n) {
                            float last = n - i == 5 ? stack[i + 4] : 0;
                            if (horizontal)
                                curveTo(stack[i], 0, stack[i + 1], stack[i + 2], last, stack[i + 3]);
                            else
                                curveTo(0, stack[i], stack[i + 1], stack[i + 2], stack[i + 3], last);
                            horizontal = !horizontal;
                            i += 4;
                        }
                        n = 0;
                        break;
                    }
                    case 10:
                    case 29: {
                        const CffIndex& subrs = b == 10 ? local : face.globalSubrs;
                        if (n > 0 && subrs.count > 0) {
                            int index = (int)stack[--n] + bias(subrs.count);
                            const u8* begin;
                            const u8* finish;
                            if (index >= 0 && subrs.get((u32)index, begin, finish))
                                run(begin, finish, depth + 1);
                        }
                        break;
                    }
                    case 11:
                        return;
                    case 14:
                        done = true;
                        break;
                    case 12: {
                        int op = p < end ? *p++ : 0;
                        if (op == 35 && n >= 12) {  // flex
                            curveTo(stack[0], stack[1], stack[2], stack[3], stack[4], stack[5]);
                            curveTo(stack[6], stack[7], stack[8], stack[9], stack[10], stack[11]);
                        } else if (op == 34 && n >= 7) {  // hflex
                            curveTo(stack[0], 0, stack[1], stack[2], stack[3], 0);
                            curveTo(stack[4], 0, stack[5], -stack[2], stack[6], 0);
                        } else if (op == 36 && n >= 9) {  // hflex1
                            curveTo(stack[0], stack[1], stack[2], stack[3], stack[4], 0);
                            curveTo(stack[5], 0, stack[6], stack[7], stack[8], -(stack[1] + stack[3] + stack[7]));
                        } else if (op == 37 && n >= 11) {  // flex1
                            float dx = stack[0] + stack[2] + stack[4] + stack[6] + stack[8];
                            float dy = stack[1] + stack[3] + stack[5] + stack[7] + stack[9];
                            curveTo(stack[0], stack[1], stack[2], stack[3], stack[4], stack[5]);
                            if (fabsf(dx) > fabsf(dy))
                                curveTo(stack[6], stack[7], stack[8], stack[9], stack[10], -dy);
                            else
                                curveTo(stack[6], stack[7], stack[8], stack[9], -dx, stack[10]);
                        }
                        n = 0;
                        break;
                    }
                    default:
                        n = 0;
                        break;
                }
            }
        }
    };

    void cffContours(int glyph, float scale, std::vector<std::vector<Point>>& out) const {
        const u8* begin;
        const u8* end;
        if (glyph < 0 || !charStrings.get((u32)glyph, begin, end))
            return;
        int group = groupOf(glyph);
        static const CffIndex none;
        CffPen pen{*this, group < (int)localSubrs.size() ? localSubrs[(size_t)group] : none, scale, out};
        pen.run(begin, end, 0);
        pen.finish();
    }

    int glyphIndex(u32 codepoint) const {
        int tables = be16(cmap + 2);
        const u8* best = nullptr;
        for (int i = 0; i < tables; i++) {
            const u8* record = cmap + 4 + i * 8;
            int platform = be16(record), encoding = be16(record + 2);
            const u8* sub = cmap + be32(record + 4);
            int format = be16(sub);
            bool unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
            if (!unicode)
                continue;
            if (format == 12 || (format == 4 && !best))
                best = sub;
        }
        if (!best)
            return 0;
        if (be16(best) == 12) {
            u32 groups = be32(best + 12);
            u32 low = 0, high = groups;
            while (low < high) {
                u32 mid = (low + high) / 2;
                const u8* g = best + 16 + mid * 12;
                if (codepoint < be32(g))
                    high = mid;
                else if (codepoint > be32(g + 4))
                    low = mid + 1;
                else
                    return (int)(be32(g + 8) + codepoint - be32(g));
            }
            return 0;
        }
        if (codepoint > 0xFFFF)
            return 0;
        int segments = be16(best + 6) / 2;
        const u8* ends = best + 14;
        const u8* starts = ends + segments * 2 + 2;
        const u8* deltas = starts + segments * 2;
        const u8* offsets = deltas + segments * 2;
        for (int i = 0; i < segments; i++) {
            if (codepoint > be16(ends + i * 2))
                continue;
            int start = be16(starts + i * 2);
            if (codepoint < (u32)start)
                return 0;
            int offset = be16(offsets + i * 2);
            int delta = bes16(deltas + i * 2);
            if (offset == 0)
                return (int)((codepoint + delta) & 0xFFFF);
            int glyph = be16(offsets + i * 2 + offset + (codepoint - start) * 2);
            return glyph == 0 ? 0 : (glyph + delta) & 0xFFFF;
        }
        return 0;
    }

    float advance(int glyph) const {
        int index = std::min(glyph, numHMetrics - 1);
        if ((u32)(index * 4 + 2) > hmtxSize || index < 0)
            return 0;
        return (float)be16(hmtx + index * 4);
    }

    void contours(int glyph, float scale, float dx, float dy, float m00, float m01, float m10, float m11, std::vector<std::vector<Point>>& out, int depth) const {
        if (charStrings.count > 0) {
            cffContours(glyph, scale, out);
            return;
        }
        if (depth > 5)
            return;
        u32 start, end;
        if (longLoca) {
            if ((u32)(glyph + 2) * 4 > locaSize)
                return;
            start = be32(loca + glyph * 4);
            end = be32(loca + glyph * 4 + 4);
        } else {
            if ((u32)(glyph + 2) * 2 > locaSize)
                return;
            start = be16(loca + glyph * 2) * 2u;
            end = be16(loca + glyph * 2 + 2) * 2u;
        }
        if (start >= end || end > glyfSize)
            return;
        const u8* p = glyf + start;
        int count = bes16(p);
        if (count < 0) {
            const u8* c = p + 10;
            int flags;
            do {
                flags = be16(c);
                int index = be16(c + 2);
                c += 4;
                float ox = 0, oy = 0;
                if (flags & CompositeWords) {
                    ox = bes16(c);
                    oy = bes16(c + 2);
                    c += 4;
                } else {
                    ox = (signed char)c[0];
                    oy = (signed char)c[1];
                    c += 2;
                }
                if (!(flags & CompositeXyValues))
                    ox = oy = 0;
                float a = 1, b = 0, cc = 0, d = 1;
                if (flags & CompositeScale) {
                    a = d = bes16(c) / 16384.0f;
                    c += 2;
                } else if (flags & CompositeXyScale) {
                    a = bes16(c) / 16384.0f;
                    d = bes16(c + 2) / 16384.0f;
                    c += 4;
                } else if (flags & CompositeMatrix) {
                    a = bes16(c) / 16384.0f;
                    b = bes16(c + 2) / 16384.0f;
                    cc = bes16(c + 4) / 16384.0f;
                    d = bes16(c + 6) / 16384.0f;
                    c += 8;
                }
                contours(index, scale, dx + (m00 * ox + m01 * oy) * scale, dy + (m10 * ox + m11 * oy) * scale, m00 * a + m01 * b, m00 * cc + m01 * d, m10 * a + m11 * b, m10 * cc + m11 * d, out, depth + 1);
            } while (flags & CompositeMore);
            return;
        }
        if (count == 0)
            return;

        const u8* ends = p + 10;
        int points = be16(ends + (count - 1) * 2) + 1;
        const u8* q = ends + count * 2;
        int instructions = be16(q);
        q += 2 + instructions;

        std::vector<u8> flags((size_t)points);
        for (int i = 0; i < points;) {
            u8 flag = *q++;
            flags[(size_t)i++] = flag;
            if (flag & 8) {
                int repeat = *q++;
                while (repeat-- > 0 && i < points)
                    flags[(size_t)i++] = flag;
            }
        }
        std::vector<int> xs((size_t)points), ys((size_t)points);
        int value = 0;
        for (int i = 0; i < points; i++) {
            u8 f = flags[(size_t)i];
            if (f & 2) {
                int d = *q++;
                value += f & 16 ? d : -d;
            } else if (!(f & 16)) {
                value += bes16(q);
                q += 2;
            }
            xs[(size_t)i] = value;
        }
        value = 0;
        for (int i = 0; i < points; i++) {
            u8 f = flags[(size_t)i];
            if (f & 4) {
                int d = *q++;
                value += f & 32 ? d : -d;
            } else if (!(f & 32)) {
                value += bes16(q);
                q += 2;
            }
            ys[(size_t)i] = value;
        }

        int first = 0;
        for (int c = 0; c < count; c++) {
            int last = be16(ends + c * 2);
            std::vector<Point> contour;
            for (int i = first; i <= last && i < points; i++) {
                float x = (float)xs[(size_t)i], y = (float)ys[(size_t)i];
                contour.push_back(Point{dx + (m00 * x + m01 * y) * scale, dy + (m10 * x + m11 * y) * scale, (flags[(size_t)i] & 1) != 0});
            }
            if (!contour.empty())
                out.push_back(std::move(contour));
            first = last + 1;
        }
    }
};

Font::Font() = default;
Font::~Font() = default;

void Font::addFace(const u8* data, size_t size) {
    auto face = std::make_unique<Face>();
    face->data = data;
    face->dataSize = size;
    if (face->load())
        _faces.push_back(std::move(face));
}

bool Font::hasFaces() const { return !_faces.empty(); }

const Font::Glyph& Font::glyph(u32 codepoint, float size, bool bold, int subpixel) {
    u64 key = (u64)codepoint | (u64)(u32)(size * 4) << 32 | (u64)(bold ? 1 : 0) << 52 | (u64)subpixel << 53;
    auto found = _cache.find(key);
    if (found != _cache.end())
        return found->second;

    if (_cacheBytes > (32u << 20)) {
        _cache.clear();
        _cacheBytes = 0;
    }

    Glyph glyph;
    const Face* face = nullptr;
    int index = 0;
    for (auto& candidate : _faces) {
        index = candidate->glyphIndex(codepoint);
        if (index != 0) {
            face = candidate.get();
            break;
        }
    }
    if (!face && !_faces.empty())
        face = _faces[0].get();

    if (face) {
        float scale = size / face->unitsPerEm;
        glyph.advance = face->advance(index) * scale + (bold ? size * 0.02f : 0);

        std::vector<std::vector<Point>> outline;
        face->contours(index, scale, 0, 0, 1, 0, 0, 1, outline, 0);
        if (!outline.empty()) {
            float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
            for (auto& contour : outline) {
                for (auto& p : contour) {
                    minX = std::min(minX, p.x);
                    maxX = std::max(maxX, p.x);
                    minY = std::min(minY, p.y);
                    maxY = std::max(maxY, p.y);
                }
            }
            float shift = subpixel * 0.25f;
            float boldShift = bold ? std::max(0.5f, size * 0.035f) : 0;
            int left = (int)floorf(minX + shift);
            int top = (int)ceilf(maxY);
            int w = (int)ceilf(maxX + shift + boldShift) - left + 1;
            int h = top - (int)floorf(minY) + 1;
            if (w > 0 && h > 0 && w < 2048 && h < 2048) {
                std::vector<float> area((size_t)(w + 2) * (size_t)h, 0.0f);
                auto rasterize = [&](float extra) {
                    std::fill(area.begin(), area.end(), 0.0f);
                    auto toPixel = [&](Point p) { return Point{p.x + shift + extra - left, top - p.y, p.on}; };
                    for (auto& contour : outline) {
                        size_t n = contour.size();
                        if (n < 2)
                            continue;
                        size_t begin = 0;
                        while (begin < n && !contour[begin].on)
                            begin++;
                        Point start;
                        if (begin == n) {
                            begin = 0;
                            start = Point{(contour[0].x + contour[n - 1].x) / 2, (contour[0].y + contour[n - 1].y) / 2, true};
                        } else {
                            start = contour[begin];
                            begin++;
                        }
                        Point current = toPixel(start);
                        Point control{0, 0, false};
                        bool hasControl = false;
                        for (size_t step = 0; step < n; step++) {
                            Point next = toPixel(contour[(begin + step) % n]);
                            if (next.on) {
                                if (hasControl)
                                    addQuad(area.data(), w, h, current, control, next);
                                else
                                    addLine(area.data(), w, h, current.x, current.y, next.x, next.y);
                                current = next;
                                hasControl = false;
                            } else {
                                if (hasControl) {
                                    Point mid{(control.x + next.x) / 2, (control.y + next.y) / 2, true};
                                    addQuad(area.data(), w, h, current, control, mid);
                                    current = mid;
                                }
                                control = next;
                                hasControl = true;
                            }
                        }
                        Point close = toPixel(start);
                        if (hasControl)
                            addQuad(area.data(), w, h, current, control, close);
                        else
                            addLine(area.data(), w, h, current.x, current.y, close.x, close.y);
                    }
                };

                glyph.left = left;
                glyph.top = top;
                glyph.w = w;
                glyph.h = h;
                glyph.coverage.assign((size_t)w * h, 0);
                for (int pass = 0; pass < (bold ? 2 : 1); pass++) {
                    rasterize(pass == 0 ? 0 : boldShift);
                    for (int y = 0; y < h; y++) {
                        float sum = 0;
                        const float* row = area.data() + (size_t)y * (w + 2);
                        for (int x = 0; x < w; x++) {
                            sum += row[x];
                            float c = fabsf(sum);
                            // A touch more weight than the exact coverage, which reads as too thin on a screen.
                            int value = (int)(powf(std::min(c, 1.0f), 0.86f) * 255 + 0.5f);
                            u8& out = glyph.coverage[(size_t)y * w + x];
                            out = (u8)std::max<int>(out, value);
                        }
                    }
                }
                _cacheBytes += glyph.coverage.size() + 64;
            }
        }
    }
    return _cache.emplace(key, std::move(glyph)).first->second;
}

static u32 nextCodepoint(const std::string& text, size_t& i) {
    u8 c = (u8)text[i++];
    if (c < 0x80)
        return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    u32 value = extra == 3 ? c & 7 : extra == 2 ? c & 15 : extra == 1 ? c & 31 : 0xFFFD;
    while (extra-- > 0 && i < text.size())
        value = value << 6 | ((u8)text[i++] & 63);
    return value;
}

float Font::measure(const std::string& text, float size, bool bold) {
    float width = 0;
    for (size_t i = 0; i < text.size();)
        width += glyph(nextCodepoint(text, i), size, bold, 0).advance;
    return width;
}

std::string fitText(Font& font, const std::string& text, float size, float maxWidth, bool bold) {
    if (font.measure(text, size, bold) <= maxWidth)
        return text;
    static const std::string ellipsis = "\xE2\x80\xA6";
    float room = maxWidth - font.measure(ellipsis, size, bold);
    float width = 0;
    size_t end = 0;
    for (size_t i = 0; i < text.size();) {
        size_t at = i;
        float w = font.glyph(nextCodepoint(text, i), size, bold, 0).advance;
        if (width + w > room)
            break;
        width += w;
        end = i;
        (void)at;
    }
    while (end > 0 && text[end - 1] == ' ')
        end--;
    return text.substr(0, end) + ellipsis;
}

std::vector<std::string> wrapText(Font& font, const std::string& text, float size, float maxWidth) {
    std::vector<std::string> lines;
    std::string line;
    size_t position = 0;
    while (position <= text.size()) {
        size_t end = text.find_first_of(" \n", position);
        if (end == std::string::npos)
            end = text.size();
        std::string word = text.substr(position, end - position);
        std::string candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && font.measure(candidate, size) > maxWidth) {
            lines.push_back(line);
            line = word;
        } else {
            line = candidate;
        }
        if (end < text.size() && text[end] == '\n') {
            lines.push_back(line);
            line.clear();
        }
        position = end + 1;
    }
    if (!line.empty())
        lines.push_back(line);
    return lines;
}

// -------------------------------------------------------------------------------------------------------------------
// Drawing

Canvas::Canvas(u32* pixels, int width, int height, int strideInPixels) : _pixels(pixels), _w(width), _h(height), _stride(strideInPixels) { clearClip(); }

void Canvas::setClip(Rect clip) {
    _clipX0 = std::max(clip.x, 0);
    _clipY0 = std::max(clip.y, 0);
    _clipX1 = std::min(clip.right(), _w);
    _clipY1 = std::min(clip.bottom(), _h);
}

void Canvas::clearClip() {
    _clipX0 = _clipY0 = 0;
    _clipX1 = _w;
    _clipY1 = _h;
}

static inline u32 packed(Color c) { return (u32)c.r | (u32)c.g << 8 | (u32)c.b << 16 | 0xFF000000u; }

static inline u32 blend(u32 d, Color c, u32 alpha) {
    u32 inverse = 255 - alpha;
    u32 rb = ((d & 0xFF00FF) * inverse + ((u32)c.r | (u32)c.b << 16) * alpha + 0x800080) >> 8 & 0xFF00FF;
    u32 g = ((d >> 8 & 0xFF) * inverse + c.g * alpha + 0x80) >> 8 & 0xFF;
    return rb | g << 8 | 0xFF000000u;
}

void Canvas::clear(Color color) {
    u32 value = packed(color);
    for (int y = 0; y < _h; y++)
        std::fill_n(_pixels + (size_t)y * _stride, _w, value);
}

void Canvas::blendRow(int x, int y, int count, Color color) {
    if (y < _clipY0 || y >= _clipY1)
        return;
    int x0 = std::max(x, _clipX0), x1 = std::min(x + count, _clipX1);
    if (x0 >= x1)
        return;
    u32* row = _pixels + (size_t)y * _stride;
    if (color.a == 255) {
        std::fill(row + x0, row + x1, packed(color));
    } else {
        for (int i = x0; i < x1; i++)
            row[i] = blend(row[i], color, color.a);
    }
}

void Canvas::blendPixel(int x, int y, Color color, int coverage) {
    if (x < _clipX0 || x >= _clipX1 || y < _clipY0 || y >= _clipY1 || coverage <= 0)
        return;
    u32& p = _pixels[(size_t)y * _stride + x];
    p = blend(p, color, (u32)coverage * color.a / 255);
}

void Canvas::fill(Rect r, Color color) {
    for (int y = r.y; y < r.bottom(); y++)
        blendRow(r.x, y, r.w, color);
}

const std::vector<u8>& Canvas::cornerMask(int radius) {
    auto found = _corners.find(radius);
    if (found != _corners.end())
        return found->second;
    std::vector<u8> mask((size_t)radius * radius);
    constexpr int Samples = 4;
    for (int y = 0; y < radius; y++) {
        for (int x = 0; x < radius; x++) {
            int inside = 0;
            for (int sy = 0; sy < Samples; sy++) {
                for (int sx = 0; sx < Samples; sx++) {
                    float dx = radius - (x + (sx + 0.5f) / Samples);
                    float dy = radius - (y + (sy + 0.5f) / Samples);
                    if (dx * dx + dy * dy <= (float)radius * radius)
                        inside++;
                }
            }
            mask[(size_t)y * radius + x] = (u8)(inside * 255 / (Samples * Samples));
        }
    }
    return _corners.emplace(radius, std::move(mask)).first->second;
}

void Canvas::fillRound(Rect r, int radius, Color color) {
    radius = std::min(radius, std::min(r.w, r.h) / 2);
    if (radius <= 0) {
        fill(r, color);
        return;
    }
    const std::vector<u8>& mask = cornerMask(radius);
    for (int y = 0; y < r.h; y++) {
        int py = r.y + y;
        int fromEdge = y < radius ? y : y >= r.h - radius ? r.h - 1 - y : -1;
        if (fromEdge < 0) {
            blendRow(r.x, py, r.w, color);
            continue;
        }
        for (int x = 0; x < radius; x++) {
            int coverage = mask[(size_t)fromEdge * radius + x];
            blendPixel(r.x + x, py, color, coverage);
            blendPixel(r.right() - 1 - x, py, color, coverage);
        }
        blendRow(r.x + radius, py, r.w - radius * 2, color);
    }
}

void Canvas::strokeRound(Rect r, int radius, int thickness, Color color) {
    radius = std::min(radius, std::min(r.w, r.h) / 2);
    int innerRadius = std::max(radius - thickness, 0);
    const std::vector<u8>* outer = radius > 0 ? &cornerMask(radius) : nullptr;
    const std::vector<u8>* inner = innerRadius > 0 ? &cornerMask(innerRadius) : nullptr;

    for (int y = 0; y < r.h; y++) {
        int py = r.y + y;
        bool topBand = y < thickness, bottomBand = y >= r.h - thickness;
        int fromEdge = y < radius ? y : y >= r.h - radius ? r.h - 1 - y : -1;

        // The straight parts.
        if (fromEdge >= 0) {
            if (topBand || bottomBand)
                blendRow(r.x + radius, py, r.w - radius * 2, color);
        } else {
            blendRow(r.x, py, thickness, color);
            blendRow(r.right() - thickness, py, thickness, color);
            continue;
        }

        // The corners: what's in the outer curve but not in the inner one.
        for (int x = 0; x < radius; x++) {
            int coverage = outer ? (*outer)[(size_t)fromEdge * radius + x] : 255;
            int ix = x - thickness, iy = fromEdge - thickness;
            int hole = 0;
            if (ix >= 0 && iy >= 0)
                hole = ix < innerRadius && iy < innerRadius && inner ? (*inner)[(size_t)iy * innerRadius + ix] : 255;
            int value = std::max(coverage - hole, 0);
            blendPixel(r.x + x, py, color, value);
            blendPixel(r.right() - 1 - x, py, color, value);
        }
    }
}

void Canvas::fillCircle(float cx, float cy, float radius, Color color) {
    int x0 = (int)floorf(cx - radius - 1), x1 = (int)ceilf(cx + radius + 1);
    int y0 = (int)floorf(cy - radius - 1), y1 = (int)ceilf(cy + radius + 1);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
            int coverage = (int)(std::min(std::max(radius - d + 0.5f, 0.0f), 1.0f) * 255);
            blendPixel(x, y, color, coverage);
        }
    }
}

void Canvas::line(float x0, float y0, float x1, float y1, float thickness, Color color) {
    int minX = (int)floorf(std::min(x0, x1) - thickness), maxX = (int)ceilf(std::max(x0, x1) + thickness);
    int minY = (int)floorf(std::min(y0, y1) - thickness), maxY = (int)ceilf(std::max(y0, y1) + thickness);
    float dx = x1 - x0, dy = y1 - y0;
    float length2 = std::max(dx * dx + dy * dy, 0.0001f);
    float half = thickness / 2;
    for (int y = minY; y <= maxY; y++) {
        for (int x = minX; x <= maxX; x++) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float t = std::min(std::max((px * dx + py * dy) / length2, 0.0f), 1.0f);
            float ex = px - t * dx, ey = py - t * dy;
            float d = sqrtf(ex * ex + ey * ey);
            int coverage = (int)(std::min(std::max(half - d + 0.5f, 0.0f), 1.0f) * 255);
            blendPixel(x, y, color, coverage);
        }
    }
}

void Canvas::image(const Image& source, int x, int y, int radius) {
    radius = std::min(radius, std::min(source.w, source.h) / 2);
    const std::vector<u8>* mask = radius > 0 ? &cornerMask(radius) : nullptr;
    for (int sy = 0; sy < source.h; sy++) {
        int py = y + sy;
        if (py < _clipY0 || py >= _clipY1)
            continue;
        int fromEdge = sy < radius ? sy : sy >= source.h - radius ? source.h - 1 - sy : -1;
        const u32* in = source.pixels.data() + (size_t)sy * source.w;
        u32* out = _pixels + (size_t)py * _stride;
        for (int sx = 0; sx < source.w; sx++) {
            int px = x + sx;
            if (px < _clipX0 || px >= _clipX1)
                continue;
            u32 p = in[sx];
            u32 alpha = p >> 24;
            if (fromEdge >= 0 && mask) {
                int cx = sx < radius ? sx : sx >= source.w - radius ? source.w - 1 - sx : -1;
                if (cx >= 0)
                    alpha = alpha * (*mask)[(size_t)fromEdge * radius + cx] / 255;
            }
            if (alpha == 255)
                out[px] = p | 0xFF000000u;
            else if (alpha > 0)
                out[px] = blend(out[px], Color{(u8)p, (u8)(p >> 8), (u8)(p >> 16), 255}, alpha);
        }
    }
}

void Canvas::shadow(Rect r, int radius, int spread, Color color) {
    for (int i = spread; i > 0; i -= 2) {
        Rect ring{r.x - i, r.y - i + spread / 2, r.w + i * 2, r.h + i * 2};
        fillRound(ring, radius + i, color.withAlpha(255 * 2 / std::max(spread, 1)));
    }
}

float Canvas::text(Font& font, float x, float baseline, const std::string& text, float size, Color color, bool bold) {
    int py = (int)(baseline + 0.5f);
    for (size_t i = 0; i < text.size();) {
        u32 codepoint = nextCodepoint(text, i);
        float rounded = floorf(x);
        int subpixel = (int)((x - rounded) * 4);
        const Font::Glyph& glyph = font.glyph(codepoint, size, bold, subpixel);
        int gx = (int)rounded + glyph.left;
        int gy = py - glyph.top;
        if (gy < _clipY1 && gy + glyph.h > _clipY0 && gx < _clipX1 && gx + glyph.w > _clipX0) {
            for (int y = std::max(0, _clipY0 - gy); y < glyph.h && gy + y < _clipY1; y++) {
                const u8* coverage = glyph.coverage.data() + (size_t)y * glyph.w;
                u32* out = _pixels + (size_t)(gy + y) * _stride + gx;
                for (int xx = std::max(0, _clipX0 - gx); xx < glyph.w && gx + xx < _clipX1; xx++) {
                    u32 c = coverage[xx];
                    if (c)
                        out[xx] = blend(out[xx], color, c * color.a / 255);
                }
            }
        }
        x += glyph.advance;
    }
    return x;
}

void Canvas::textFit(Font& font, float x, float baseline, const std::string& value, float size, float maxWidth, Color color, bool bold) {
    text(font, x, baseline, fitText(font, value, size, maxWidth, bold), size, color, bold);
}

void Canvas::textRight(Font& font, float right, float baseline, const std::string& value, float size, Color color, bool bold) {
    text(font, right - font.measure(value, size, bold), baseline, value, size, color, bold);
}

void Canvas::textCenter(Font& font, float center, float baseline, const std::string& value, float size, Color color, bool bold) {
    text(font, center - font.measure(value, size, bold) / 2, baseline, value, size, color, bold);
}

float Canvas::textWrapped(Font& font, float x, float baseline, const std::string& value, float size, float maxWidth, float lineHeight, Color color, int maxLines) {
    int count = 0;
    for (auto& line : wrapText(font, value, size, maxWidth)) {
        if (maxLines > 0 && count >= maxLines)
            break;
        text(font, x, baseline, line, size, color);
        baseline += lineHeight;
        count++;
    }
    return baseline;
}

}  // namespace hbp
