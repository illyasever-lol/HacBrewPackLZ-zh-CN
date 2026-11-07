// Everything on screen is drawn with this, in plain memory. No GPU, no drivers and nothing to go wrong, and because it's only memory,
// the same code that runs on a console also runs on a PC, where its screens can be looked at and tested.
#pragma once

#include <unordered_map>

#include "../core/common.h"

namespace hbp {

struct Color {
    u8 r = 0, g = 0, b = 0, a = 255;
    Color withAlpha(int alpha) const { return Color{r, g, b, (u8)(a * alpha / 255)}; }
};

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    int right() const { return x + w; }
    int bottom() const { return y + h; }
    bool contains(int px, int py) const { return px >= x && py >= y && px < right() && py < bottom(); }
};

struct Image {
    int w = 0, h = 0;
    std::vector<u32> pixels;  // RGBA, a byte each, in memory order
    bool empty() const { return pixels.empty(); }
    /// Averages it down to a smaller size, which is what keeps icons crisp.
    Image scaledTo(int width, int height) const;
};

/// A TrueType font, or several: a character missing from one is looked for in the next. Glyphs are drawn once, then remembered.
class Font {
public:
    Font();
    ~Font();
    /// The data has to stay where it is for as long as the font is used. A font it can't make sense of is left out.
    void addFace(const u8* data, size_t size);
    bool hasFaces() const;

    float measure(const std::string& text, float size, bool bold = false);

    struct Glyph {
        int left = 0, top = 0, w = 0, h = 0;
        float advance = 0;
        std::vector<u8> coverage;
    };
    const Glyph& glyph(u32 codepoint, float size, bool bold, int subpixel);

private:
    struct Face;
    std::vector<std::unique_ptr<Face>> _faces;
    std::unordered_map<u64, Glyph> _cache;
    size_t _cacheBytes = 0;
};

class Canvas {
public:
    Canvas(u32* pixels, int width, int height, int strideInPixels);

    int width() const { return _w; }
    u32* row(int y) { return _pixels + (size_t)y * _stride; }
    int height() const { return _h; }

    void setClip(Rect clip);
    void clearClip();

    void clear(Color color);
    void fill(Rect r, Color color);
    void fillRound(Rect r, int radius, Color color);
    void strokeRound(Rect r, int radius, int thickness, Color color);
    void fillCircle(float cx, float cy, float radius, Color color);
    void line(float x0, float y0, float x1, float y1, float thickness, Color color);
    void image(const Image& image, int x, int y, int radius = 0);
    /// Soft shadow under a rounded card.
    void shadow(Rect r, int radius, int spread, Color color);

    /// Draws from a baseline. Returns where it ended.
    float text(Font& font, float x, float baseline, const std::string& text, float size, Color color, bool bold = false);
    /// Cut short with an ellipsis if it doesn't fit.
    void textFit(Font& font, float x, float baseline, const std::string& text, float size, float maxWidth, Color color, bool bold = false);
    void textRight(Font& font, float right, float baseline, const std::string& text, float size, Color color, bool bold = false);
    void textCenter(Font& font, float center, float baseline, const std::string& text, float size, Color color, bool bold = false);
    /// Wraps at spaces to fit the width. Returns the baseline after the last line.
    float textWrapped(Font& font, float x, float baseline, const std::string& text, float size, float maxWidth, float lineHeight, Color color, int maxLines = 0);

private:
    void blendRow(int x, int y, int count, Color color);
    void blendPixel(int x, int y, Color color, int coverage);
    const std::vector<u8>& cornerMask(int radius);

    u32* _pixels;
    int _w, _h, _stride;
    int _clipX0, _clipY0, _clipX1, _clipY1;
    std::unordered_map<int, std::vector<u8>> _corners;
};

std::string fitText(Font& font, const std::string& text, float size, float maxWidth, bool bold = false);
std::vector<std::string> wrapText(Font& font, const std::string& text, float size, float maxWidth);

}  // namespace hbp
