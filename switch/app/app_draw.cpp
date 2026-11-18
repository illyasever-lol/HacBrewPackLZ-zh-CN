// What's on screen. Everything is drawn again from what's known whenever something changes, so there's no screen state to get out of step
// with the work going on behind it.
#include "app_internal.h"

#include <cmath>

#include "../third_party/qrcodegen/qrcodegen.h"

namespace hbp {

using namespace app;

namespace {

struct Theme {
    Color background, card, raised, focus, text, dim, faint, line, accent, onAccent, warn, bad, scrim;
    /// What's taken on a bar of space: quieter than the accent colour, which is kept for what can be freed.
    Color used;
    /// A control of a few choices side by side, and the choice that's made, filled in.
    Color segmentBack, segmentOn;
};

const Theme DarkTheme = {
    {27, 29, 32},    {37, 40, 44},   {50, 54, 59},    {38, 52, 53},  {238, 240, 242}, {163, 170, 178}, {112, 119, 127},
    {52, 56, 61},    {45, 190, 172}, {8, 30, 27},     {232, 170, 66}, {238, 110, 94},  {0, 0, 0, 160},  {84, 90, 97},     {42, 45, 49}, {69, 75, 82},
};

const Theme LightTheme = {
    {240, 241, 243}, {255, 255, 255}, {226, 228, 232}, {222, 242, 239}, {28, 32, 36}, {88, 96, 104}, {136, 143, 150},
    {214, 217, 221}, {0, 134, 120},   {255, 255, 255}, {170, 110, 10},  {198, 56, 42}, {0, 0, 0, 110},  {172, 178, 185},  {226, 228, 232}, {255, 255, 255},
};

// Where each part of a row goes.
constexpr int TickX = 66;
constexpr int IconX = 92;
constexpr int IconSize = 60;
constexpr int TextX = 168;
constexpr int TextW = 500;
// "至少节省" is far wider than the figures under it, so its column takes the room it needs, and the headings stand apart alike.
constexpr int NowRight = 780;
constexpr int AfterRight = 890;
constexpr int SavedRight = 1050;
constexpr int BarX = 1082;
constexpr int BarRight = 1234;

/// A rough idea of a length of time, which is all anyone can say about time left.
std::string roughTime(double seconds) {
    if (seconds < 60)
        return "不到一分钟";
    int minutes = (int)(seconds / 60 + 0.5);
    if (minutes < 60)
        return format("%d 分钟", minutes);
    return format("%d 小时 %d 分钟", minutes / 60, minutes % 60);
}

/// Where the next character of some UTF-8 starts.
size_t nextCharacter(const std::string& text, size_t position) {
    position++;
    while (position < text.size() && ((u8)text[position] & 0xC0) == 0x80)
        position++;
    return position;
}

/// One colour over another, as an opaque colour, so shapes drawn in it can overlap without a seam.
Color mixColor(Color base, Color over, int alpha) {
    auto mix = [&](u8 a, u8 b) { return (u8)((a * (255 - alpha) + b * alpha + 127) / 255); };
    return Color{mix(base.r, over.r), mix(base.g, over.g), mix(base.b, over.b), 255};
}

/// What goes on a tile when a game has no icon to show: the first letter of its first two words.
std::string initialsOf(const std::string& name) {
    std::string initials;
    bool atStart = true;
    for (size_t i = 0; i < name.size() && initials.size() < 8;) {
        size_t next = nextCharacter(name, i);
        char c = name[i];
        if (c == ' ') {
            atStart = true;
        } else if (atStart) {
            initials += name.substr(i, next - i);
            atStart = false;
            if (initials.size() >= 2 && (u8)initials[0] < 0x80)
                break;
            if ((u8)initials[0] >= 0x80)
                break;
        }
        i = next;
    }
    return initials;
}

}  // namespace
// -------------------------------------------------------------------------------------------------------------------
// Pieces

/// Words that explain something, wrapped over as many lines as there's room for. Anything that still doesn't fit ends in an ellipsis rather
/// than stopping mid-sentence, and is noted, so the tests can say so.
float App::drawExplanation(Canvas& c, float x, float baseline, const std::string& text, float size, float width, float lineHeight, Color color, int maxLines) {
    auto lines = wrapText(_font, text, size, width);
    if (maxLines > 0 && (int)lines.size() > maxLines) {
        _cut.push_back(text);
        lines.resize((size_t)maxLines);
        std::string last = lines.back() + "\xE2\x80\xA6";
        if (_font.measure(last, size) > width)
            last = fitText(_font, last, size, width);
        lines.back() = last;
    }
    for (auto& line : lines) {
        c.text(_font, x, baseline, line, size, color);
        baseline += lineHeight;
    }
    return baseline;
}

/// As many lines as the longest description takes, up to four, so none is cut short and the list above doesn't move from one to the next.
int App::descriptionLines(const std::vector<OptionItem>& items, float width) {
    size_t most = 2;
    for (auto& item : items)
        most = std::max(most, wrapText(_font, item.help, 15, width).size());
    return (int)std::min<size_t>(most, 4);
}

void App::draw(Canvas& c) {
    struct Timed {
        double& out;
        double started;
        ~Timed() { out = (sys::now() - started) * 1000; }
    } timed{_lastDrawMs, sys::now()};
    const Theme& t = _dark ? DarkTheme : LightTheme;
    c.clearClip();
    _cut.clear();
    _fitCut.clear();
    _crowded.clear();
    _headings.clear();
    setCutLog(&_fitCut);
    _lettersForIcons.clear();
    if (drawOverDialogBehind(c))
        return;
    // Whatever asks for another frame while what's behind a question is drawn is still moving, and isn't kept.
    bool wanted = _dirty.exchange(false);
    if (drawOverMenuBehind(c)) {
        if (wanted)
            _dirty = true;
        return;
    }
    c.clear(t.background);
    _hits.clear();
    switch (_screen) {
        case Screen::Main:
            drawMain(c);
            break;
        case Screen::Job:
            drawJob(c);
            break;
        case Screen::Results:
            drawResults(c);
            break;
        case Screen::NoKeys:
            drawNoKeys(c);
            break;
        case Screen::Browse:
            drawBrowse(c);
            break;
        case Screen::Welcome:
            drawWelcome(c);
            break;
        case Screen::Settings:
            drawSettings(c);
            break;
        case Screen::Details:
            drawDetails(c);
            break;
        case Screen::Inside:
            drawInside(c);
            break;
    }
    // A new screen fades in from the background, briefly.
    if (_screenSince >= 0 && _time - _screenSince < FadeTime) {
        c.fill(Rect{0, 0, ScreenW, ScreenH}, t.background.withAlpha((int)(255 * (1 - easeOut((_time - _screenSince) / FadeTime)))));
        _dirty = true;
    }
    if (!_toast.empty())
        drawToast(c);
    if (_menu) {
        drawMenuBehind(c);
        c.keep(_menuBehind);
        _menuBehindPage = _menu->page;
        _menuBehindItems = (int)_menu->items.size();
        _menuBehindStatus.clear();
        drawMenu(c);
    } else if (!_menuBehind.empty()) {
        std::vector<u32>().swap(_menuBehind);
    }
    if (_dialog)
        drawDialogOver(c, wanted);
    else if (!_dialogBehind.empty())
        std::vector<u32>().swap(_dialogBehind);
    if (wanted)
        _dirty = true;
}

/// While a question rises, what's behind it stays as it was, darkened, and only the question is drawn: drawing it all again for every frame
/// of that is far more work than the question, and is what made questions drop frames on the console. Only for that moment, so nothing
/// behind is ever left out of date for longer. False if there's nothing kept for it.
bool App::drawOverDialogBehind(Canvas& c) {
    if (!_dialog || _dialogBehindFor != _dialog->serial || !dialogRising() || _dialogBehind.size() != (size_t)c.width() * c.height())
        return false;
    c.restore(_dialogBehind);
    drawDialog(c);
    return true;
}

void App::drawDialogOver(Canvas& c, bool behindMoving) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    // The screen darkens at once, as it does under the menu, and only the question moves.
    c.fill(Rect{0, 0, ScreenW, ScreenH}, t.scrim);
    // Something moving under it, such as a list still sliding, goes on being drawn.
    behindMoving = behindMoving || _dirty || _tabShown != position() || _focusShown != _focus[_tab] || _scrollShown != _scroll[_tab];
    if (dialogRising() && !behindMoving) {
        c.keep(_dialogBehind);
        _dialogBehindFor = _dialog->serial;
    } else if (!_dialogBehind.empty()) {
        std::vector<u32>().swap(_dialogBehind);
        _dialogBehindFor = 0;
    }
    drawDialog(c);
}

bool App::dialogRising() const { return _dialogSince >= 0 && _time - _dialogSince < RiseTime; }

/// With the + menu open, everything behind it stays as it was until it closes, but for the line at the bottom saying what's going on.
/// False if there's nothing kept from before to draw it over.
bool App::drawOverMenuBehind(Canvas& c) {
    if (!_menu || _menuBehindPage != _menu->page || _menuBehindItems != (int)_menu->items.size() || _menuBehind.empty())
        return false;
    const Theme& t = _dark ? DarkTheme : LightTheme;
    auto restore = [&] { c.restore(_menuBehind); };
    if (_screen == Screen::Main) {
        sys::Lock lock(_mutex);
        std::string status = format("%d %d %d %d %d %d %d ", (int)_jobKind.load(), _analystRunning ? 1 : 0, _analystMeasuring ? 1 : 0, _analysisIndex, _jobIndex, _jobCount,
                                    _scanningTab.load()) + _analysisTitle;
        if (status != _menuBehindStatus) {
            _menuBehindStatus = status;
            Rect strip{0, FooterY, ScreenW, ScreenH - FooterY};
            restore();
            c.setClip(strip);
            c.fill(strip, t.background);
            if (_overview)
                drawOverviewFooter(c);
            else
                drawFooter(c, visibleRowsLocked(_tab));
            c.fill(strip, t.scrim.withAlpha(150));
            c.clearClip();
            for (int y = FooterY; y < c.height(); y++)
                memcpy(_menuBehind.data() + (size_t)y * c.width(), c.row(y), (size_t)c.width() * sizeof(u32));
        }
    }
    restore();
    drawMenu(c);
    if (_dialog)
        drawDialogOver(c, false);
    return true;
}

void App::drawToast(Canvas& c) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    constexpr float Size = 17;
    constexpr int PadX = 22, Height = 42;
    int width = (int)_font.measure(_toast, Size) + PadX * 2;
    double shown = easeOut((_time - _toastSince) / FadeTime);
    if (_toastUntil > 0)
        shown = std::min(shown, std::max(0.0, (_toastUntil - _time) / 0.2));
    if (shown < 1)
        _dirty = true;
    int alpha = (int)(255 * shown);
    Rect pill{(ScreenW - width) / 2, FooterY - Height - 12 + (int)((1 - shown) * 8), width, Height};
    c.shadow(pill, 14, 14, Color{0, 0, 0, (u8)(70 * shown)});
    c.fillRound(pill, Height / 2, t.raised.withAlpha(alpha));
    c.textCenter(_font, ScreenW / 2.0f, pill.y + 27.0f, _toast, Size, t.text.withAlpha(alpha));
}

void App::drawButtonGlyph(Canvas& c, float cx, float cy, const char* name, float radius, Color fill, Color ink) {
    size_t length = strlen(name);
    if (!strcmp(name, "L") || !strcmp(name, "R") || !strcmp(name, "ZL") || !strcmp(name, "ZR")) {
        int w = length == 1 ? 26 : 32;
        c.fillRound(Rect{(int)cx - w / 2, (int)cy - 10, w, 20}, 6, fill);
        c.textCenter(_font, cx, cy + 5, name, 13.5f, ink, true);
        return;
    }
    c.fillCircle(cx, cy, radius, fill);
    float arm = radius * 0.5f;
    if (length == 2 && name[1] == 'S') {
        c.textCenter(_font, cx, cy + radius * 0.34f, name, radius * 0.84f, ink, true);
    } else if (!strcmp(name, "+")) {
        c.line(cx - arm, cy, cx + arm, cy, 2.2f, ink);
        c.line(cx, cy - arm, cx, cy + arm, 2.2f, ink);
    } else if (!strcmp(name, "-")) {
        c.line(cx - arm, cy, cx + arm, cy, 2.2f, ink);
    } else if (!strcmp(name, "<>")) {
        float w = radius * 0.3f;
        c.line(cx - w, cy - arm * 0.9f, cx - w - arm * 0.8f, cy, 2.0f, ink);
        c.line(cx - w - arm * 0.8f, cy, cx - w, cy + arm * 0.9f, 2.0f, ink);
        c.line(cx + w, cy - arm * 0.9f, cx + w + arm * 0.8f, cy, 2.0f, ink);
        c.line(cx + w + arm * 0.8f, cy, cx + w, cy + arm * 0.9f, 2.0f, ink);
    } else {
        c.textCenter(_font, cx, cy + radius * 0.42f, name, radius * 1.16f, ink, true);
    }
}

void App::drawHints(Canvas& c, const std::vector<Hint>& hints) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    constexpr float Radius = 13;
    constexpr float LabelSize = 16;
    constexpr float Gap = 18;
    float x = ScreenW - 24;
    for (auto it = hints.rbegin(); it != hints.rend(); ++it) {
        float labelW = _font.measure(it->label, LabelSize);
        float start = x - labelW - 8 - Radius * 2;
        Color ink = it->enabled ? t.text : t.faint;
        drawButtonGlyph(c, start + Radius, (float)HintY, it->button, Radius, ink, t.background);
        c.text(_font, start + Radius * 2 + 8, HintY + LabelSize * 0.36f, it->label, LabelSize, ink);
        if (it->enabled)
            _hits.push_back(Hit{Rect{(int)start - 8, FooterY + 4, (int)(x - start) + 16, ScreenH - FooterY - 4}, it->action});
        x = start - Gap;
    }
}

void App::drawTick(Canvas& c, float cx, float cy, float radius, bool ticked, bool enabled) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    if (ticked) {
        c.fillCircle(cx, cy, radius, t.accent);
        float s = radius / 13;
        c.line(cx - 6 * s, cy + 0.5f * s, cx - 1.5f * s, cy + 5 * s, 2.6f * s, t.onAccent);
        c.line(cx - 1.5f * s, cy + 5 * s, cx + 6.5f * s, cy - 4.5f * s, 2.6f * s, t.onAccent);
    } else {
        int r = (int)radius;
        c.strokeRound(Rect{(int)cx - r, (int)cy - r, r * 2, r * 2}, r, 2, enabled ? t.faint : t.line);
    }
}

void App::drawTile(Canvas& c, const std::shared_ptr<Image>& icon, Color tile, const std::string& name, int x, int y, int size, int radius) {
    if (icon && icon->w == size && icon->h == size) {
        c.image(*icon, x, y, radius);
        return;
    }
    if (icon && !icon->empty())
        _lettersForIcons.push_back(name);
    c.fillRound(Rect{x, y, size, size}, radius, tile);
    float textSize = size * 0.38f;
    c.textCenter(_font, x + size / 2.0f, y + size / 2.0f + textSize * 0.36f, initialsOf(name), textSize, Color{255, 255, 255}, true);
}

void App::drawSpinner(Canvas& c, float cx, float cy, float radius) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    constexpr int Dots = 8;
    int head = (int)(_time * 10) % Dots;
    for (int i = 0; i < Dots; i++) {
        double angle = i * 2 * 3.14159265 / Dots;
        int behind = (head - i + Dots) % Dots;
        c.fillCircle(cx + (float)cos(angle) * radius, cy + (float)sin(angle) * radius, radius * 0.24f, t.accent.withAlpha(255 - behind * 30));
    }
}

void App::drawBar(Canvas& c, Rect r, double fraction, Color fill) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    c.fillRound(r, r.h / 2, t.raised);
    fraction = std::max(0.0, std::min(1.0, fraction));
    if (fraction > 0) {
        Rect part = r;
        part.w = std::max(r.h, (int)(r.w * fraction + 0.5));
        c.fillRound(part, r.h / 2, fill);
    }
}

/// One green, stronger the more a game saves: brighter on the dark theme and deeper on the light one, so either way the games that save most
/// stand out most from the track, and the order reads by lightness alone, for everyone.
Color App::shareColor(double share) const {
    static const Color QuietDark{39, 116, 72}, StrongDark{52, 199, 120};
    static const Color QuietLight{132, 194, 150}, StrongLight{24, 150, 82};
    // Most games come to between 40% and 90% of their size: a saving of 10% or less is the quietest, and one of 60% or more the strongest.
    double saved = 1 - std::min(std::max(share, 0.0), 1.0);
    double strength = std::min(std::max((saved - 0.10) / 0.50, 0.0), 1.0);
    int alpha = (int)(strength * 255 + 0.5);
    return _dark ? mixColor(QuietDark, StrongDark, alpha) : mixColor(QuietLight, StrongLight, alpha);
}

void App::drawShareBar(Canvas& c, Rect r, double share, bool faint) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    c.fillRound(r, r.h / 2, t.raised);
    // What it saves is the coloured part, so the games that gain most stand out most, in green.
    double saved = 1 - std::min(std::max(share, 0.0), 1.0);
    if (saved <= 0)
        return;
    Color color = shareColor(share);
    if (faint)
        color = color.withAlpha(120);
    Rect part = r;
    part.w = std::min(r.w, std::max(r.h, (int)(r.w * saved + 0.5)));
    c.fillRound(part, r.h / 2, color);
}

// -------------------------------------------------------------------------------------------------------------------
// The list

void App::drawStorage(Canvas& c, int x, int width, const char* label, i64 free, i64 total, i64 frees) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    if (free < 0 || total <= 0)
        return;
    c.text(_font, (float)x, 30, label, 14, t.dim);
    c.textRight(_font, (float)(x + width), 30, formatSize(free) + " 可用", 14, t.text);
    drawMeterBar(c, Rect{x, 40, width, 5}, free, total, frees);
}

/// The same on every screen: grey for what's taken, turning amber when it's nearly full, and at its end, in the accent colour, the part
/// that compressing what's ticked would free.
void App::drawMeterBar(Canvas& c, Rect bar, i64 free, i64 total, i64 frees) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    int radius = bar.h / 2;
    c.fillRound(bar, radius, t.raised);
    double used = std::max(0.0, std::min(1.0, 1.0 - (double)free / (double)total));
    double freed = std::max(0.0, std::min(used, (double)frees / (double)total));
    int usedW = (int)lround(bar.w * used);
    int keptW = (int)lround(bar.w * (used - freed));
    if (usedW <= 0)
        return;
    usedW = std::max(usedW, bar.h);
    Color taken = used > 0.92 ? t.warn : t.used;
    if (keptW >= usedW) {
        c.fillRound(Rect{bar.x, bar.y, usedW, bar.h}, radius, taken);
        return;
    }
    c.fillRound(Rect{bar.x, bar.y, usedW, bar.h}, radius, t.accent);
    if (keptW <= 0)
        return;
    // Where what's kept meets what's freed is a straight edge, with a hairline between them.
    if (keptW > radius * 2) {
        c.fillRound(Rect{bar.x, bar.y, keptW, bar.h}, radius, taken);
        c.fill(Rect{bar.x + keptW - radius, bar.y, radius, bar.h}, taken);
    } else {
        c.fill(Rect{bar.x + radius, bar.y, std::max(keptW - radius, 0), bar.h}, taken);
    }
    c.fill(Rect{bar.x + keptW, bar.y, 2, bar.h}, t.background);
}

void App::drawTopBar(Canvas& c) {
    const Theme& t = _dark ? DarkTheme : LightTheme;

    // The lists, as one round control of a few choices, the way the views of each list are under it, only bigger: the one shown is filled in,
    // and slides from one to the next. L and R, which go through them, are either side of it, and each can be tapped.
    constexpr float TabSize = 18;
    constexpr int PillH = 44, Inset = 4, Pad = 20, Middle = TopBarH / 2;
    constexpr int Positions = TabCount + 1;
    const char* names[Positions] = {"概览", "已安装游戏", "游戏文件"};
    int xs[Positions], widths[Positions];
    int here = position();
    int total = Inset * 2;
    // Each as wide as it is in bold, so none moves as the one shown changes.
    for (int i = 0; i < Positions; i++) {
        widths[i] = (int)_font.measure(names[i], TabSize, true) + Pad * 2;
        total += widths[i];
    }
    // No box around them, unlike the views under them: the lists are places to go, and the views a control on the list that's shown. The
    // one shown is filled in the same round way, so the two read as one family, a level apart.
    Rect pill{Margin + 36, Middle - PillH / 2, total, PillH};
    int x = pill.x + Inset;
    for (int i = 0; i < Positions; i++) {
        xs[i] = x;
        x += widths[i];
    }
    int from = std::min((int)_tabShown, Positions - 1), to = std::min(from + 1, Positions - 1);
    float part = (float)_tabShown - from;
    Rect shown{(int)(xs[from] + (xs[to] - xs[from]) * part), pill.y + Inset, (int)(widths[from] + (widths[to] - widths[from]) * part), PillH - Inset * 2};
    // On the screen's own background, the light theme's white would hardly show, so it's filled a shade darker there.
    c.fillRound(shown, shown.h / 2, _dark ? t.segmentOn : t.segmentBack);
    for (int i = 0; i < Positions; i++) {
        c.textCenter(_font, xs[i] + widths[i] / 2.0f, Middle + 6.5f, names[i], TabSize, here == i ? t.text : t.dim, here == i);
        _hits.push_back(Hit{Rect{xs[i], 0, widths[i], TopBarH}, [this, i] { goToPosition(i); }});
    }
    drawButtonGlyph(c, pill.x - 22.0f, (float)Middle, "L", 0, t.raised, t.dim);
    drawButtonGlyph(c, pill.right() + 22.0f, (float)Middle, "R", 0, t.raised, t.dim);

    // The overview shows these large, with what can be freed, so they'd only say it twice there.
    if (_overview)
        return;
    i64 sd = freedOnLocked(Storage::Sd), console = freedOnLocked(Storage::Nand);
    if (_freeUsb >= 0) {
        drawStorage(c, 770, 140, "SD 卡", _freeSd, _totalSd, sd);
        drawStorage(c, 930, 140, "主机内存", _freeNand, _totalNand, console);
        drawStorage(c, 1090, 140, "USB", _freeUsb, _totalUsb, 0);
    } else {
        drawStorage(c, 830, 190, "SD 卡", _freeSd, _totalSd, sd);
        drawStorage(c, 1050, 190, "主机内存", _freeNand, _totalNand, console);
    }
}

void App::drawColumns(Canvas& c, const std::vector<Row*>& rows) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    constexpr float Size = 14;
    constexpr float Middle = TopBarH + ColumnsH / 2.0f;
    constexpr float Baseline = Middle + 5;

    int ticked = 0, tickable = 0;
    for (auto* row : rows) {
        if (row->blocked.empty()) {
            tickable++;
            ticked += row->ticked ? 1 : 0;
        }
    }
    if (tickable > 0) {
        bool all = ticked == tickable;
        drawTick(c, TickX, Middle, 9, all, true);
        if (!all && ticked > 0)
            c.line(TickX - 4.0f, Middle, TickX + 4.0f, Middle, 2.2f, t.accent);
        _hits.push_back(Hit{Rect{TickX - 22, TopBarH, 44, ColumnsH}, [this, all] { tickAll(!all); }});
    }

    c.fill(Rect{0, ListTop - 1, ScreenW, 1}, t.line);

    // The views of the list, with how many games each has, as one control over the game column, the same kind as the lists' above, only
    // smaller, as it's one level down. Left and right on the D-pad, which go through them, are shown after it, as R is after the lists. Each
    // can be tapped too.
    constexpr float ViewSize = 16;
    constexpr int PillH = 36, Inset = 4, Pad = 18;
    auto views = viewsOffered();
    std::vector<std::string> labels;
    std::vector<int> widths;
    int total = Inset * 2;
    for (View view : views) {
        int count = 0;
        for (auto& row : _rows)
            count += row.tab == _tab && inViewLocked(row, view) ? 1 : 0;
        labels.push_back(format("%s %d", viewName(view), count));
        widths.push_back((int)_font.measure(labels.back(), ViewSize, true) + Pad * 2);
        total += widths.back();
    }
    Rect pill{IconX, (int)(Middle - PillH / 2.0f), total, PillH};
    c.fillRound(pill, PillH / 2, t.segmentBack);
    c.strokeRound(pill, PillH / 2, 1, t.line);
    // The one shown, filled in, sliding to it from the one before, as the lists' does above.
    std::vector<int> starts;
    int target = 0;
    for (size_t i = 0, at = pill.x + Inset; i < views.size(); at += widths[i], i++) {
        starts.push_back(at);
        if (views[i] == _view[_tab])
            target = (int)i;
    }
    double& slid = _viewShown[_tab];
    if (slid < 0 || slid > (double)views.size() - 1)
        slid = target;
    slid += (target - slid) * _glideStep;
    if (std::abs(slid - target) < 0.004)
        slid = target;
    else
        _dirty = true;
    if (!views.empty()) {
        int from = std::min((int)slid, (int)views.size() - 1), to = std::min(from + 1, (int)views.size() - 1);
        double part = slid - from;
        Rect on{(int)(starts[(size_t)from] + (starts[(size_t)to] - starts[(size_t)from]) * part), pill.y + Inset,
                (int)(widths[(size_t)from] + (widths[(size_t)to] - widths[(size_t)from]) * part), PillH - Inset * 2};
        c.fillRound(on, on.h / 2, t.segmentOn);
    }
    int x = pill.x + Inset;
    for (size_t i = 0; i < views.size(); i++) {
        bool shown = views[i] == _view[_tab];
        Rect segment{x, pill.y + Inset, widths[i], PillH - Inset * 2};
        (void)segment;
        c.textCenter(_font, x + widths[i] / 2.0f, Middle + 6, labels[i], ViewSize, shown ? t.text : t.dim, shown);
        View view = views[i];
        _hits.push_back(Hit{Rect{x, TopBarH, widths[i], ColumnsH}, [this, view] { setView(view); }});
        x += widths[i];
    }
    constexpr float Radius = 12;
    drawButtonGlyph(c, pill.right() + 10 + Radius, Middle, "<>", Radius, t.raised, t.dim);

    // The column it's sorted by says so itself, in the accent colour with an arrow, rather than a line of words that reads like one more
    // column. By name needs no saying.
    if (rows.empty())
        return;
    auto column = [&](float right, const char* label, bool sorted) {
        _headings.push_back(label);
        if (!sorted) {
            c.textRight(_font, right, Baseline, label, Size, t.dim);
            return;
        }
        c.textRight(_font, right - 14, Baseline, label, Size, t.accent, true);
        c.line(right - 10, Middle - 2, right - 5.5f, Middle + 3, 2, t.accent);
        c.line(right - 5.5f, Middle + 3, right - 1, Middle - 2, 2, t.accent);
    };
    // Where what's saved is an estimate, it's what each is sure to save, and the column says so.
    bool atLeast = std::any_of(rows.begin(), rows.end(), [](const Row* r) { return r->hasEstimate && r->state != RowState::Done && r->estimate.atLeast(); });
    // On Done, nothing is still to come: there's what each takes, and what compressing it freed.
    // On Done, what each was, what it takes now and what was freed, in the places To do has what it takes, what it'll come to and what
    // it'll save.
    bool done = _view[_tab] == View::Done;
    column(NowRight, done ? "原来" : "现在", !done && _settings.sort == 1);
    column(AfterRight, done ? "现在" : "处理后", done && _settings.sort == 1);
    column(SavedRight, done ? "已释放" : atLeast ? "至少节省" : "已节省", _settings.sort == 0);
}

App::ShownRow App::shownLocked(const Row& row) const {
    ShownRow shown;
    bool blocked = !row.blocked.empty();
    bool compressed = isCompressed(row);
    bool toInstall = compressedToInstallLocked(row);

    // With two files of the same game on a list, which file it is matters whatever else there is to say.
    std::string whichFile;
    if (row.tab != TabInstalled && row.extracted.empty()) {
        size_t separator = row.detail.rfind("\xC2\xB7");
        if (separator != std::string::npos)
            whichFile = "  \xC2\xB7" + row.detail.substr(separator + 2);
    }

    shown.second = row.detail;
    shown.secondKind = ShownRow::Plain;
    if (blocked) {
        shown.second = row.blocked == UpdateNoNewer ? UpdateNoNewerShort : row.blocked;
        shown.secondKind = ShownRow::Faint;
    } else if (row.state == RowState::Failed) {
        // In a few plain words, which the failure as it's said, under About this game, doesn't always manage.
        shown.second = plainReason(row.message);
        shown.secondKind = ShownRow::Bad;
    } else if (row.state == RowState::Done) {
        shown.second = row.freed > 0 ? format("%s，释放 %s", _settings.compress ? "已压缩" : "已重打包", formatSize(row.freed).c_str()) : "已完成" + whichFile;
        shown.secondKind = ShownRow::Good;
    } else if (row.updateOverCopy) {
        shown.second = UpdateToMakePart;
        shown.secondKind = ShownRow::Warn;
    } else if (toInstall) {
        shown.second = "已压缩，尚未安装" + whichFile;
        shown.secondKind = ShownRow::Good;
    } else if (compressed) {
        // What compressing freed, which a copy this tool made says, or that its makers compressed it. Installed, a game that says neither
        // had nothing compressed: it's as small as it gets, as a game with all its data in its program is. A game file a version before 1.5
        // made doesn't say what it came to before, and is only known to be compressed.
        i64 freed = row.hasEstimate ? row.estimate.freedBytes : 0;
        bool installed = row.tab == TabInstalled;
        shown.second = row.hasEstimate && row.estimate.byMakers ? "发行商已自行压缩" + whichFile
                     : freed > 0 ? format(installed ? "已压缩，释放 %s" : "已压缩，小 %s", formatSize(freed).c_str()) + whichFile
                     : installed ? "无可压缩的游戏数据" : "已压缩" + whichFile;
        shown.secondKind = ShownRow::Good;
    } else if (row.hasEstimate && row.estimate.olderCopy && isDoneLocked(row)) {
        // Done as it was, until compressing it again is sure to be worth it: what it freed, and the columns what making it again would save.
        shown.second = row.estimate.freedBytes > 0 ? format("由旧版本压缩，释放 %s", formatSize(row.estimate.freedBytes).c_str()) : "由旧版本压缩";
        shown.secondKind = ShownRow::Good;
    } else if (row.hasEstimate && row.estimate.byMakers && row.belowMinimum.empty() && !row.notSure && _settings.compress) {
        // Its developer compressed it, and making it again here is worth it, which the columns say: to do, but only ever ticked by hand, as
        // its updates then need its original. Not worth it, or not sure yet, it says so as any other game does, below.
        shown.second = whichFile.empty() ? std::string("由开发者压缩：之后更新需要原始游戏") : "发行商已自行压缩" + whichFile;
        shown.secondKind = ShownRow::Warn;
    } else if (!row.belowMinimum.empty() && !(row.dlcNotInFile && row.titleId == row.application)) {
        // A game whose DLC isn't in its file keeps its details, which say so. Its Saved column says what that saves.
        shown.second = whichFile.empty() ? row.belowMinimum : row.belowMinimum.substr(0, row.belowMinimum.find(':')) + whichFile;
        shown.secondKind = ShownRow::Warn;
    } else if (row.notSure) {
        // It may save more than a first look can say. Thorough, it's read in full by itself in turn. Quick, it stays as it is unless it's ticked.
        shown.second = std::string(_settings.thorough ? "可能更省：将依次测量" : "可能更省：完整分析将测量它") + whichFile;
    } else if (row.hasEstimate && row.estimate.olderCopy) {
        // Made by a version from before repeats were stored once: what it freed then, and the columns say what making it again saves.
        shown.second = row.estimate.freedBytes > 0 ? format("由旧版本压缩，释放 %s", formatSize(row.estimate.freedBytes).c_str()) : "由旧版本压缩";
    } else if (row.hasEstimate && row.estimate.packedBytes > 0 && row.estimate.inputBytes != row.estimate.packedBytes) {
        // Being installed, an NSZ is compared with what it takes installed as it is, which its Now says, so this says what the file is.
        shown.second = format("%s 文件，%s 按原样安装", formatSize(row.estimate.packedBytes).c_str(), formatSize(row.estimate.inputBytes).c_str()) + whichFile;
    }

    shown.whichFile = whichFile;
    // A packed file being installed is as big as what it takes installed as it is, which is what it's compared with.
    shown.now = formatSize(row.hasEstimate && row.estimate.packedBytes > 0 ? row.estimate.inputBytes : row.now);
    if (row.state == RowState::Working) {
        // Read in full, it takes a while, so it says that's what's going on.
        // A game being read in full is still "测量中" while it waits for first looks in the middle of it.
        shown.status = row.hasEstimate && (!row.estimate.measured || row.measureAgain) && (_analystMeasuring || _lookingWhileMeasuring) ? "正在测量" : "正在查看";
    } else if (toInstall) {
        // Nothing to save, as it's compressed already: what's left is putting it on the console.
        shown.status = "可安装";
        shown.after = shown.now;
        shown.saved = "无可再省";
        shown.nothing = true;
    } else if (compressed) {
        // "Nothing" alone, beside what it freed and under a heading of Saved, would read as if compressing it had saved nothing.
        shown.status = "（勾选）";
        shown.after = shown.now;
        shown.saved = "无可再省";
        shown.nothing = true;
    } else if (row.notSure) {
        // A first look can't vouch for it, so there's no figure to give until it's measured.
        shown.saved = "不确定";
        shown.notSure = true;
    } else if (row.hasEstimate) {
        // What it's sure to save, and what it comes to with that, so the two add up. One that would come out larger says by how much, below
        // zero and in red, when that's a fact about it (sureToGrow).
        bool grows = row.estimate.sureToGrow(_settings.compress) && !row.estimate.olderCopy && !row.estimate.byMakers;
        // Whatever it's sure to save is said, however little: the minimum saving is what says whether that's worth it.
        if (row.estimate.sureSavedBytes() > 0 || grows) {
            shown.after = formatSize(row.estimate.mostBytes());
            shown.saved = formatSize(row.estimate.sureSavedBytes());
            // What it comes to, as a share of what it is now. Within 1% of all of it, to a tenth, as a whole number would say 100% beside
            // what it saves.
            double after = 1 - row.estimate.sureSavedFraction();
            // What it saves, as the bar under it shows, to a tenth when it's under 1%, as a whole number would say 0% beside a figure.
            double saves = 1 - after;
            shown.share = saves < 0 ? format("+%.0f%%", -saves * 100) : saves < 0.01 ? format("\xE2\x88\x92%.1f%%", saves * 100)
                                                                            : format("\xE2\x88\x92%.0f%%", saves * 100);
        } else if (_settings.compress && !row.estimate.measured && !row.estimate.alreadyCompressed) {
            // A first look can't say a game saves nothing, as for one its developer compressed, made again here: it may save more, which
            // reading it in full finds out. One all compressed already, made again to put its DLC in its file, saves nothing.
            shown.saved = "不确定";
            shown.notSure = true;
        } else {
            // Measured, or nothing to compress: it saves nothing, or only the margin of error, at the size it is. 0 B or a figure below zero
            // misled, and no figure at all looked like one still to come.
            shown.after = shown.now;
            shown.saved = "无";
            shown.nothing = true;
        }
    } else if (!blocked && row.state != RowState::Failed) {
        // Not looked at yet, so nothing is known about it, which is said in words rather than left blank.
        shown.saved = "尚未分析";
        shown.notSure = true;
    }
    return shown;
}

/// On Done, each game says what's become of it, rather than what compressing it would do: that it saves nothing worth it goes without saying
/// there, as that's why it's on Done. Its second line is its own, its version and where it is, unless something about it stands apart, and
/// the Saved column, called Freed there, has what compressing it freed.
App::ShownRow App::shownInViewLocked(const Row& row) const {
    ShownRow shown = shownLocked(row);
    if (_view[row.tab] != View::Done || row.state == RowState::Working || shown.status == "Ready to install")
        return shown;
    bool compressed = isCompressed(row);
    bool apart = !row.blocked.empty() || row.state == RowState::Failed || row.updateOverCopy;
    bool byMakers = compressed && row.hasEstimate && row.estimate.byMakers;
    bool older = row.hasEstimate && row.estimate.olderCopy;
    if (!apart) {
        if (byMakers) {
            shown.second = "发行商已自行压缩" + shown.whichFile;
            shown.secondKind = ShownRow::Good;
        } else if (older) {
            shown.second = "由旧版本压缩" + shown.whichFile;
            shown.secondKind = ShownRow::Good;
        } else {
            shown.second = row.detail;
            shown.secondKind = ShownRow::Plain;
        }
    }
    i64 freed = std::max<i64>(row.freed, row.hasEstimate ? row.estimate.freedBytes : 0);
    shown.share.clear();
    shown.nothing = false;
    shown.notSure = false;
    shown.saved = freed > 0 && (compressed || older) ? formatSize(freed) : "";
    shown.freed = !shown.saved.empty();
    shown.status = compressed ? "（勾选）" : "";
    return shown;
}

void App::drawRow(Canvas& c, const Row& row, int y) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    bool blocked = !row.blocked.empty();

    float pop = row.tickedAt >= 0 ? (float)easeOut((_time - row.tickedAt) / PopTime) : 1.0f;
    if (pop < 1)
        _dirty = true;
    drawTick(c, TickX, y + RowH / 2.0f, 13 * (0.7f + 0.3f * pop), row.ticked && !blocked, !blocked);
    drawTile(c, row.icon, row.tile, row.name, IconX, y + (RowH - IconSize) / 2, IconSize, 10);

    ShownRow shown = shownInViewLocked(row);
    bool compressed = isCompressed(row);
    Color main = blocked ? t.faint : row.ticked || compressed ? t.text : t.dim;
    // Everything in a row is placed from its middle, so it stays in the middle whatever height rows are.
    float mid = y + RowH / 2.0f;
    c.textFit(_font, TextX, mid - 5, row.name, 20, TextW, main);

    const Color kinds[] = {t.dim, t.faint, t.bad, t.accent, t.warn};
    c.textFit(_font, TextX, mid + 18, shown.second, 14.5f, TextW, kinds[shown.secondKind]);

    float number = mid + 6;
    bool onDone = _view[row.tab] == View::Done && row.state != RowState::Working && shown.status != "Ready to install";
    if (!onDone)
        c.textRight(_font, NowRight, number, shown.now, 16.5f, main);

    // On Done: what it was, what it takes now and what compressing it freed, with the bar To do has for what it will come to. A copy whose
    // size before isn't known, such as one an older version made, says so; one settled there without being compressed freed nothing.
    if (onDone) {
        i64 freed = shown.freed ? std::max<i64>(row.freed, row.hasEstimate ? row.estimate.freedBytes : 0) : 0;
        if (freed > 0) {
            double kept = (double)row.now / (double)(row.now + freed);
            c.textRight(_font, NowRight, number, formatSize(row.now + freed), 16.5f, main);
            c.textRight(_font, AfterRight, number, shown.now, 16.5f, main);
            c.textRight(_font, SavedRight, number, formatSize(freed), 16.5f, t.accent, true);
            c.textRight(_font, BarRight, mid - 4, format("\xE2\x88\x92%.0f%%", (1 - kept) * 100), 16.5f, main);
            drawShareBar(c, Rect{BarX, (int)(mid + 6), BarRight - BarX, 6}, kept, false);
        } else {
            c.textRight(_font, NowRight, number, compressed ? "未知" : shown.now, 16.5f, compressed ? t.dim : main);
            c.textRight(_font, AfterRight, number, shown.now, 16.5f, main);
            c.textRight(_font, SavedRight, number, compressed ? "未知" : "无", 16.5f, t.dim);
        }
        return;
    }
    if (row.state == RowState::Working) {
        drawSpinner(c, AfterRight - 8.0f, y + RowH / 2.0f, 9);
        c.text(_font, AfterRight + 16.0f, number - 1, shown.status, 15, t.dim);
    } else if (shown.status == "Ready to install") {
        c.textRight(_font, BarRight, number, shown.status, 16.5f, row.ticked ? t.accent : t.dim, row.ticked);
    } else if (compressed) {
        drawTick(c, BarRight - 13.0f, y + RowH / 2.0f, 13, true, true);
    } else if (shown.notSure) {
        c.textRight(_font, SavedRight, number, shown.saved, 16.5f, row.ticked ? t.text : t.dim);
    } else if (row.hasEstimate && !shown.nothing) {
        bool wanted = row.belowMinimum.empty();
        bool grows = row.estimate.savedBytes() < 0;
        Color strong = grows ? t.bad : !row.ticked ? t.dim : wanted ? t.accent : t.warn;
        c.textRight(_font, AfterRight, number, shown.after, 16.5f, main);
        c.textRight(_font, SavedRight, number, shown.saved, 16.5f, strong, true);
        // What the game comes to: the figure in the colour of the sizes, and the bar with what goes in the colour of what's saved.
        c.textRight(_font, BarRight, mid - 4, shown.share, 16.5f, grows ? t.bad : main);
        double target = 1 - row.estimate.sureSavedFraction();
        if (row.barShown < 0 || std::abs(row.barShown - target) < 0.002) {
            row.barShown = target;
        } else {
            row.barShown += (target - row.barShown) * _glideStep;
            _dirty = true;
        }
        drawShareBar(c, Rect{BarX, (int)(mid + 6), BarRight - BarX, 6}, row.barShown, !row.ticked);
    } else if (!shown.after.empty() && !shown.nothing) {
        c.textRight(_font, AfterRight, number, shown.after, 16.5f, t.faint);
        c.textRight(_font, SavedRight, number, shown.saved, 16.5f, t.faint);
    }
    // Sure to save nothing, compressed already or not: the size it stays at, and that in words, beside its tick or what's left to do.
    if (shown.nothing) {
        c.textRight(_font, AfterRight, number, shown.after, 16.5f, main);
        c.textRight(_font, SavedRight, number, shown.saved, 16.5f, blocked ? t.faint : t.dim);
    }
}

void App::drawEmpty(Canvas& c, const std::string& title, const std::string& detail, bool spinner) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    constexpr float Center = ScreenW / 2.0f;
    float y = ListTop + ListH / 2.0f - 30;
    if (spinner) {
        drawSpinner(c, Center, y - 44, 15);
    }
    c.textCenter(_font, Center, y, title, 23, t.text, true);
    if (!detail.empty()) {
        auto lines = wrapText(_font, detail, 17, 800);
        float baseline = y + 34;
        for (auto& line : lines) {
            c.textCenter(_font, Center, baseline, line, 17, t.dim);
            baseline += 25;
        }
    }
}

void App::drawMain(Canvas& c) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    sys::Lock lock(_mutex);
    drawTopBar(c);
    if (_overview) {
        drawOverview(c);
        drawOverviewFooter(c);
        return;
    }
    auto rows = visibleRowsLocked(_tab);
    drawColumns(c, rows);

    int count = (int)rows.size();
    if (count == 0) {
        drawEmptyList(c);
    } else {
        c.setClip(Rect{0, ListTop, ScreenW, ListH});
        int focusY = ListTop + (int)lround(_focusShown * RowH - _scrollShown);
        Rect focus{20, focusY + 3, ScreenW - 44, RowH - 6};
        c.fillRound(focus, 12, t.focus);
        c.strokeRound(focus, 12, 2, t.accent);

        int first = std::max(0, (int)(_scrollShown / RowH));
        int last = std::min(count - 1, (int)((_scrollShown + ListH) / RowH));
        for (int i = first; i <= last; i++) {
            int y = ListTop + i * RowH - (int)lround(_scrollShown);
            drawRow(c, *rows[(size_t)i], y);
            int top = std::max(y, ListTop), bottom = std::min(y + RowH, ListTop + ListH);
            if (bottom > top) {
                _hits.push_back(Hit{Rect{0, top, ScreenW, bottom - top}, [this, i] {
                    _focus[_tab] = i;
                    toggleRow(i);
                }});
            }
        }
        c.clearClip();

        if (count > VisibleRows) {
            double most = (double)count * RowH - ListH;
            int thumb = std::max(40, ListH * VisibleRows / count);
            int thumbY = ListTop + (int)((ListH - thumb) * (_scrollShown / most));
            c.fillRound(Rect{ScreenW - 14, thumbY, 4, thumb}, 2, t.faint);
        }
    }

    drawFooter(c, rows);
}

//// An icon made smaller once, which keeps it crisp, rather than each time it's drawn.
std::shared_ptr<Image> App::smallIcon(const std::shared_ptr<Image>& icon, int size) {
    if (!icon || icon->empty() || (icon->w == size && icon->h == size))
        return icon;
    auto found = _smallIcons.find({icon.get(), size});
    if (found != _smallIcons.end() && found->second.first.lock() == icon)
        return found->second.second;
    if (_smallIcons.size() > 128)
        _smallIcons.clear();
    auto small = std::make_shared<Image>(icon->scaledTo(size, size));
    _smallIcons[{icon.get(), size}] = {icon, small};
    return small;
}

/// The one number that matters and, beside each place games go, how full it is and how much of that compressing what's ticked would free.
/// Below that, a card for each list with what there is to do on it, the buttons that do it, the games that save the most, and what's done.
// Where the overview's parts go: the bars of free space, then a card for each list, with what's ticked, its buttons, the games under them,
// and the lines at its bottom.
constexpr int OverviewMeterTop = TopBarH + 85, OverviewMeterGap = 40, OverviewMeterH = 12;
/// Between the end of a place's bar and the sizes beside it, which the bar gives way to when they're wide.
constexpr int MeterTextGap = 24;
constexpr int OverviewCardW = 590, OverviewCardGap = 20, OverviewBottom = FooterY - 14;
constexpr int CardPad = 24, CardInset = 12, MainLineH = 70, SecondLineH = 42, SummaryH = 96;
constexpr int ButtonH = 48, ActW = 300, ButtonGap = 16, OtherW = 226;
/// How far below the middle of a card the button goes that a reason has above it.
constexpr int ButtonBelowMiddle = 30;

/// Where each card goes, and every place on it the pointer stops. A card with something ticked has its button that does it and the one
/// that opens the list to choose, under what's ticked, then the games under them there's room for, then the lines at its bottom, which each
/// card has in the same place.
App::OverviewLayout App::overviewLayout(const Overview& overview) const {
    OverviewLayout layout;
    int meters = (int)overview.meters.size();
    int top = meters == 0 ? TopBarH + 88 : OverviewMeterTop + (meters - 1) * OverviewMeterGap + OverviewMeterH + 27;
    auto add = [&](OverviewStop::Kind kind, int tab, int line, int game, const std::string& key, Rect rect) {
        OverviewStop stop;
        stop.kind = kind;
        stop.tab = tab;
        stop.line = line;
        stop.game = game;
        stop.key = key;
        stop.rect = rect;
        layout.stops.push_back(stop);
    };
    for (int tab = 0; tab < TabCount; tab++) {
        const OverviewCard& card = overview.cards[tab];
        Rect area{40 + tab * (OverviewCardW + OverviewCardGap), top, OverviewCardW, OverviewBottom - top};
        bool hasMain = !card.lines.empty() && OverviewLine::atTop(card.lines[0].kind);
        int seconds = (int)card.lines.size() - (hasMain ? 1 : 0);
        int secondTop = area.bottom() - 12 - seconds * SecondLineH;
        Rect body{area.x + CardInset, area.y + 50, area.w - CardInset * 2, (seconds > 0 ? secondTop - 6 : area.bottom() - 16) - (area.y + 50)};
        layout.cards[tab] = area;
        layout.bodies[tab] = body;
        layout.secondTop[tab] = secondTop;
        layout.summaries[tab] = Rect{};
        int primary = primaryLine(card);
        int left = area.x + CardPad, width = area.w - CardPad * 2;
        if (hasMain && card.lines[0].kind == OverviewLine::ToDo) {
            // What's to do, its figures and its bar; then what's good to know, each a line the pointer stops on; then the buttons, at the
            // bottom, the one that does it wider than the one that opens the list to choose, together as wide as the card. Each is in the
            // same place on both cards.
            layout.summaries[tab] = Rect{left, area.y + 50, width, SummaryH};
            int buttons = area.bottom() - 18 - ButtonH;
            int notes = area.y + 50 + SummaryH + 12;
            layout.secondTop[tab] = notes;
            int fit = std::max(0, (buttons - 10 - notes) / SecondLineH);
            for (int i = 0; i < seconds && i < fit; i++)
                add(OverviewStop::Line, tab, 1 + i, -1, "", Rect{body.x, notes + i * SecondLineH, body.w, SecondLineH});
            if (primary >= 0) {
                int act = (width - ButtonGap) * 60 / 100;
                add(OverviewStop::Act, tab, primary, -1, "", Rect{left, buttons, act, ButtonH});
                add(OverviewStop::Choose, tab, 0, -1, "", Rect{left + act + ButtonGap, buttons, width - act - ButtonGap, ButtonH});
            } else {
                add(OverviewStop::Choose, tab, 0, -1, "", Rect{left, buttons, width, ButtonH});
            }
            continue;
        }
        // A card's one button goes where the other card's buttons are, at the bottom, as wide as the card, and its lines above it.
        int buttons = area.bottom() - 18 - ButtonH;
        bool button = (hasMain && card.lines[0].sources) || (!hasMain && primary >= 0);
        if (button) {
            secondTop = buttons - 10 - seconds * SecondLineH;
            layout.secondTop[tab] = secondTop;
            layout.bodies[tab].h = std::max(0, (seconds > 0 ? secondTop - 6 : buttons - 10) - body.y);
        }
        if (hasMain && card.lines[0].sources) {
            // No game files at all: the reason, and under it the button that says where they're looked for.
            add(OverviewStop::Sources, tab, 0, -1, "", Rect{left, buttons, width, ButtonH});
        } else if (hasMain) {
            // Nothing to count: the reason fills the card.
            add(OverviewStop::Line, tab, 0, -1, "", body);
        } else if (primary >= 0) {
            // Everything here is done, but what's compressed can still be installed, which is what the button does.
            add(OverviewStop::Act, tab, primary, -1, "", Rect{left, buttons, width, ButtonH});
        }
        for (int i = 0; i < seconds; i++)
            add(OverviewStop::Line, tab, (hasMain ? 1 : 0) + i, -1, "", Rect{body.x, secondTop + i * SecondLineH, body.w, SecondLineH});
    }
    return layout;
}

/// What the pointer was on, if it's still there: the same button on the same card, the same game, or the same line. Otherwise the button
/// that does what the card is there for, or whatever else is on the card, or the other card's button.
int App::overviewStopIndex(const Overview& overview, const OverviewLayout& layout) const {
    int sameKind = -1, sameLine = -1, act = -1, sameCard = -1, anyAct = -1;
    for (int i = 0; i < (int)layout.stops.size(); i++) {
        const OverviewStop& stop = layout.stops[(size_t)i];
        if (stop.kind == OverviewStop::Act && anyAct < 0)
            anyAct = i;
        if (stop.tab != _overviewAt.tab)
            continue;
        if (stop.kind == OverviewStop::Game && !_overviewAt.game.empty() && stop.key == _overviewAt.game)
            return i;
        if (sameCard < 0)
            sameCard = i;
        if (stop.kind == OverviewStop::Act && act < 0)
            act = i;
        bool button = stop.kind == OverviewStop::Act || stop.kind == OverviewStop::Choose || stop.kind == OverviewStop::Sources;
        if (button && stop.kind == _overviewAt.stop && sameKind < 0)
            sameKind = i;
        if (stop.kind == OverviewStop::Line && _overviewAt.stop == OverviewStop::Line && sameLine < 0) {
            OverviewLine::Kind kind = overview.cards[stop.tab].lines[(size_t)stop.line].kind;
            if (kind == _overviewAt.kind || (OverviewLine::atTop(kind) && OverviewLine::atTop(_overviewAt.kind)))
                sameLine = i;
        }
    }
    if (sameKind >= 0)
        return sameKind;
    if (sameLine >= 0)
        return sameLine;
    if (act >= 0)
        return act;
    if (sameCard >= 0)
        return sameCard;
    return anyAct >= 0 ? anyAct : 0;
}

/// Up and down go to what's nearest on the same card that way, keeping to the same column where there's one there. Left and right go to
/// what's beside it on the same row, or else straight across to the other card, to what's nearest in height, and of those the one nearest
/// the side it comes in from.
int App::nextOverviewStop(const OverviewLayout& layout, int from, int move, int across) {
    const OverviewStop& at = layout.stops[(size_t)from];
    auto midY = [](const Rect& r) { return r.y + r.h / 2; };
    auto midX = [](const Rect& r) { return r.x + r.w / 2; };
    int best = -1;
    long long bestScore = 0;
    auto consider = [&](int i, long long score) {
        if (best < 0 || score < bestScore) {
            best = i;
            bestScore = score;
        }
    };
    if (move != 0) {
        for (int i = 0; i < (int)layout.stops.size(); i++) {
            const OverviewStop& stop = layout.stops[(size_t)i];
            int ahead = (midY(stop.rect) - midY(at.rect)) * move;
            if (i == from || stop.tab != at.tab || ahead <= 0)
                continue;
            bool sameColumn = stop.rect.x < at.rect.right() && at.rect.x < stop.rect.right();
            consider(i, (long long)ahead * 4 + (sameColumn ? 0 : abs(midX(stop.rect) - midX(at.rect))));
        }
        return best >= 0 ? best : from;
    }
    if (across == 0)
        return from;
    for (int i = 0; i < (int)layout.stops.size(); i++) {
        const OverviewStop& stop = layout.stops[(size_t)i];
        bool sameRow = stop.rect.y < at.rect.bottom() && at.rect.y < stop.rect.bottom();
        int ahead = (midX(stop.rect) - midX(at.rect)) * across;
        if (i != from && stop.tab == at.tab && sameRow && ahead > 0)
            consider(i, ahead);
    }
    if (best >= 0)
        return best;
    int other = at.tab + across;
    if (other < 0 || other >= TabCount)
        return from;
    int side = across > 0 ? layout.cards[other].x : layout.cards[other].right();
    for (int i = 0; i < (int)layout.stops.size(); i++) {
        const OverviewStop& stop = layout.stops[(size_t)i];
        if (stop.tab == other)
            consider(i, (long long)abs(midY(stop.rect) - midY(at.rect)) * 4 + abs(midX(stop.rect) - side));
    }
    return best >= 0 ? best : from;
}

void App::drawOverview(Canvas& c) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    c.fill(Rect{0, TopBarH, ScreenW, 1}, t.line);
    const Overview& overview = overviewLocked();

    // In line with the cards under it.
    constexpr int Left = Margin, Right = ScreenW - Margin;
    constexpr int BarX = 168, BarW = 832, BarH = OverviewMeterH, MeterGap = OverviewMeterGap;
    c.text(_font, Left, TopBarH + 52.0f, overview.headline, 30, t.text, true);
    bool freeing = std::any_of(overview.meters.begin(), overview.meters.end(), [](const OverviewMeter& m) { return m.frees > 0; });
    if (freeing) {
        // What the accent colour on the bars stands for, said once.
        const char* key = "可释放";
        float width = _font.measure(key, 14);
        c.fillRound(Rect{(int)(Right - width) - 22, TopBarH + 39, 12, 12}, 3, t.accent);
        c.textRight(_font, (float)Right, TopBarH + 50.0f, key, 14, t.dim);
    }

    // Free now, and free afterwards if anything's to be freed here: the second is what stands out.
    struct Part {
        std::string text;
        Color color;
        bool bold;
    };
    std::vector<std::vector<Part>> said;
    std::vector<float> widths;
    for (auto& meter : overview.meters) {
        std::vector<Part> parts;
        if (meter.frees > 0)
            parts = {{formatSize(meter.free), t.dim, false}, {"  \xE2\x86\x92  ", t.faint, false}, {formatSize(meter.free + meter.frees), t.accent, true}, {" 可用", t.dim, false}};
        else
            parts = {{formatSize(meter.free), t.text, false}, {" 可用", t.dim, false}};
        float width = 0;
        for (auto& part : parts)
            width += _font.measure(part.text, 16, part.bold);
        said.push_back(std::move(parts));
        widths.push_back(width);
    }
    // The bars end where the widest of those begins, so no figure runs into one.
    float widest = widths.empty() ? 0 : *std::max_element(widths.begin(), widths.end());
    int barW = std::max(100, std::min(BarW, Right - MeterTextGap - (int)widest - 1 - BarX));
    int barY = OverviewMeterTop;
    for (size_t i = 0; i < overview.meters.size(); i++) {
        auto& meter = overview.meters[i];
        float baseline = barY + 11.0f;
        c.text(_font, Left, baseline, meter.label, 17, t.text);
        drawMeterBar(c, Rect{BarX, barY, barW, BarH}, meter.free, meter.total, meter.frees);
        float x = Right - widths[i];
        if (x < BarX + barW + MeterTextGap)
            _crowded.push_back(std::string(meter.label) + "'s sizes");
        for (auto& part : said[i])
            x = c.text(_font, x, baseline, part.text, 16, part.color, part.bold);
        barY += MeterGap;
    }

    OverviewLayout layout = overviewLayout(overview);
    int focus = overviewStopIndex(overview, layout);
    drawOverviewCard(c, TabInstalled, overview.cards[0], layout, focus);
    drawOverviewCard(c, TabFiles, overview.cards[1], layout, focus);
}

/// One list: its name and size, then what's ticked to do on it and what that saves, with the button that does it, the one that opens the
/// list to choose, and the games under them that save the most. At the bottom, what's left to install and what's done. A list with nothing
/// to count says why instead. The button X does is filled in on the card the pointer's on, with X on it, and only outlined on the other.
void App::drawOverviewCard(Canvas& c, int tab, const OverviewCard& card, const OverviewLayout& layout, int focus) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    static const char* const ListNames[TabCount] = {"已安装游戏", "游戏文件"};
    Rect area = layout.cards[tab], body = layout.bodies[tab];
    c.fillRound(area, 16, t.card);
    c.text(_font, (float)(area.x + CardPad), area.y + 36.0f, ListNames[tab], 19, t.text, true);
    if (card.games > 0) {
        std::string size = format("%d 个游戏  \xC2\xB7  %s", card.games, formatSize(card.bytes).c_str());
        c.textRight(_font, (float)(area.right() - CardPad), area.y + 36.0f, size, 15, t.dim);
    }
    bool lead = focus >= 0 && focus < (int)layout.stops.size() && layout.stops[(size_t)focus].tab == tab;

    auto focusAt = [&](Rect r, bool focused) {
        if (focused) {
            c.fillRound(r, 12, t.focus);
            c.strokeRound(r, 12, 2, t.accent);
        }
    };
    auto chevron = [&](Rect r, bool focused) {
        float x = r.right() - 22.0f, mid = r.y + r.h / 2.0f;
        Color ink = focused ? t.accent : t.faint;
        c.line(x - 6, mid - 7, x + 1, mid, 2.2f, ink);
        c.line(x + 1, mid, x - 6, mid + 7, 2.2f, ink);
    };

    // What's ticked, and what it saves, which the button under it does. The pointer doesn't stop on it: the buttons are what do something.
    bool hasMain = !card.lines.empty() && OverviewLine::atTop(card.lines[0].kind);
    int seconds = (int)card.lines.size() - (hasMain ? 1 : 0);
    if (hasMain && card.lines[0].kind == OverviewLine::ToDo) {
        // What's to do, what the games are now and come to, what that frees, and as a bar, what they save, as each row has it.
        const OverviewLine& line = card.lines[0];
        Rect r = layout.summaries[tab];
        c.textFit(_font, (float)r.x, r.y + 28.0f, line.text, 21, (float)r.w, t.text, true);
        std::string saves;
        if (line.saved > 0)
            saves = std::string(line.atLeast ? (tab == TabInstalled && replacesInstalled() ? "至少释放 " : "至少节省 ")
                                            : (tab == TabInstalled && replacesInstalled() ? "释放 " : "节省 ")) + formatSize(line.saved);
        float savesW = saves.empty() ? 0 : _font.measure(saves, 16, true);
        if (!line.sub.empty())
            c.textFit(_font, (float)r.x, r.y + 56.0f, line.sub, 16, r.w - savesW - (saves.empty() ? 0 : 16.0f), t.dim);
        if (!saves.empty())
            c.textRight(_font, (float)r.right(), r.y + 56.0f, saves, 16, t.accent, true);
        if (line.share > 0.005) {
            std::string share = format("\xE2\x88\x92%.0f%%", line.share * 100);
            float shareW = _font.measure(share, 16);
            double& shown = _cardShareShown[tab];
            double target = 1 - line.share;
            if (shown < 0 || std::abs(shown - target) < 0.002) {
                shown = target;
            } else {
                shown += (target - shown) * _glideStep;
                _dirty = true;
            }
            drawShareBar(c, Rect{r.x, r.y + 74, (int)(r.w - shareW - 14), 8}, shown);
            c.textRight(_font, (float)r.right(), r.y + 83.0f, share, 16, t.text);
        }
    } else if (hasMain && card.lines[0].sources) {
        // The reason, in the middle of what's above the button that says where to look.
        drawOverviewReason(c, body, card.lines[0]);
    } else if (!hasMain && seconds > 0) {
        // Everything on the list is done, or only waits to be installed, which the button under it does.
        float center = body.x + body.w / 2.0f, mid = body.y + body.h / 2.0f;
        drawTick(c, center, mid - 22, 22, true, true);
        // A game that won't start isn't done with, whatever compressing has left, which the line under it says.
        bool wontStart = std::any_of(card.lines.begin(), card.lines.end(), [](const OverviewLine& line) { return line.kind == OverviewLine::WontStart; });
        const char* said = !wontStart ? "此处内容已全部处理" : _settings.compress ? "这里没有可压缩的游戏" : "这里没有可重打包的游戏";
        c.textCenter(_font, center, mid + 30, said, 20, t.text, true);
    }

    for (int index = 0; index < (int)layout.stops.size(); index++) {
        const OverviewStop& stop = layout.stops[(size_t)index];
        if (stop.tab != tab)
            continue;
        const OverviewLine& line = card.lines[(size_t)stop.line];
        bool focused = index == focus;
        Rect r = stop.rect;
        switch (stop.kind) {
            case OverviewStop::Act:
                drawOverviewButton(c, r, actLabel(line), true, lead, focused);
                break;
            case OverviewStop::Choose:
                drawOverviewButton(c, r, "选择游戏", false, lead, focused);
                break;
            case OverviewStop::Sources:
                drawOverviewButton(c, r, "搜索位置", false, lead, focused);
                break;
            default:
                if (OverviewLine::atTop(line.kind)) {
                    // Nothing to count: the reason fills the card, and A on it opens the list all the same.
                    focusAt(r, focused);
                    drawOverviewReason(c, r, line);
                    break;
                }
                focusAt(r, focused);
                float glyph = r.x + 31.0f, mid = r.y + SecondLineH / 2.0f;
                if (line.kind == OverviewLine::Done) {
                    drawTick(c, glyph, mid, 11, true, true);
                } else if (line.kind == OverviewLine::NotSure && _analystRunning && _analystMeasuring && _analyzingTab == line.tab) {
                    // Going on now.
                    drawSpinner(c, glyph, mid, 9);
                } else if (line.kind == OverviewLine::NotSure) {
                    // A question in a ring: what a first look couldn't say.
                    c.strokeRound(Rect{(int)glyph - 10, (int)mid - 10, 20, 20}, 10, 2, t.accent);
                    c.textCenter(_font, glyph, mid + 5.5f, "?", 15, t.accent, true);
                } else if (line.kind == OverviewLine::WontStart || line.kind == OverviewLine::Developer) {
                    // Something wrong, which can be put right, or worth knowing before it's done: an exclamation mark in a ring, in the colour
                    // of a warning.
                    c.strokeRound(Rect{(int)glyph - 10, (int)mid - 10, 20, 20}, 10, 2, t.warn);
                    c.textCenter(_font, glyph, mid + 5.5f, "!", 15, t.warn, true);
                } else {
                    // Down into the console: an arrow in a ring.
                    c.strokeRound(Rect{(int)glyph - 10, (int)mid - 10, 20, 20}, 10, 2, t.accent);
                    c.line(glyph, mid - 5.5f, glyph, mid + 5, 2.0f, t.accent);
                    c.line(glyph - 4, mid + 1.5f, glyph, mid + 5.5f, 2.0f, t.accent);
                    c.line(glyph + 4, mid + 1.5f, glyph, mid + 5.5f, 2.0f, t.accent);
                }
                if (line.kind == OverviewLine::ToInstall || line.kind == OverviewLine::WontStart) {
                    // A on it installs them, or offers to fix them, which it says where the arrow to open the list would be.
                    const char* does = line.kind == OverviewLine::ToInstall ? "安装" : "修复";
                    float word = _font.measure(does, 16, true);
                    c.textFit(_font, r.x + 52.0f, mid + 6, line.text, 18, r.w - 52 - 40.0f - word, t.text);
                    c.textRight(_font, (float)(r.right() - 20), mid + 6, does, 16, t.accent, true);
                } else if (line.kind == OverviewLine::NotSure) {
                    // How long it has left, where the arrow would be, worked out as it's drawn, as it goes down while a game is read. A on it
                    // says what it is.
                    std::string sub = _settings.thorough && !line.sub.empty() ? measureTimeLocked(line.tab) : line.sub;
                    float left = sub.empty() ? 0 : _font.measure(sub, 15);
                    c.textFit(_font, r.x + 52.0f, mid + 6, line.text, 18, r.w - 52 - 40.0f - left, t.text);
                    if (!sub.empty())
                        c.textRight(_font, (float)(r.right() - 20), mid + 6, sub, 15, t.dim);
                } else {
                    c.textFit(_font, r.x + 52.0f, mid + 6, line.text, 18, r.w - 52 - 50.0f, t.text);
                    chevron(r, focused);
                }
                break;
        }
        // A tap puts the pointer there, and a second one presses it.
        _hits.push_back(Hit{r, [this, stop, line, focused] {
            pointOverviewAt(stop, line);
            _dirty = true;
            if (focused)
                pressOverviewStop(stop, line);
        }});
    }
    if (seconds > 0)
        c.fill(Rect{area.x + CardPad, layout.secondTop[tab] - 6, area.w - CardPad * 2, 1}, t.line);
}

/// A button that says what it does. The one that does what the card is there for is filled in where the pointer is, with X on it, as X does
/// it from anywhere on the card, and only outlined on the other card, so there's one to look to. The pointer on a button is a ring around it.
void App::drawOverviewButton(Canvas& c, Rect r, const std::string& label, bool primary, bool lead, bool focused) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    bool filled = primary && lead;
    if (filled) {
        c.fillRound(r, 12, t.accent);
    } else if (primary) {
        c.strokeRound(r, 12, 2, t.accent);
    } else {
        c.fillRound(r, 12, t.raised);
    }
    Color ink = filled ? t.onAccent : primary ? t.accent : t.text;
    constexpr float Size = 17, Radius = 12, GlyphGap = 10;
    float width = _font.measure(label, Size, true) + (filled ? Radius * 2 + GlyphGap : 0);
    float x = r.x + (r.w - width) / 2.0f, mid = r.y + r.h / 2.0f;
    if (filled) {
        drawButtonGlyph(c, x + Radius, mid, "X", Radius, t.onAccent, t.accent);
        x += Radius * 2 + GlyphGap;
    }
    c.text(_font, x, mid + Size * 0.36f, label, Size, ink, true);
    if (focused)
        c.strokeRound(Rect{r.x - 5, r.y - 5, r.w + 10, r.h + 10}, 16, 2.5f, t.text);
}

void App::drawOverviewReason(Canvas& c, Rect r, const OverviewLine& line) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    auto detail = line.sub.empty() ? std::vector<std::string>() : wrapText(_font, line.sub, 15, r.w - 80.0f);
    if (detail.size() > 3)
        detail.resize(3);
    float height = 28 + detail.size() * 22.0f + (line.kind == OverviewLine::Looking ? 54 : 0);
    float y = r.y + (r.h - height) / 2 + (line.kind == OverviewLine::Looking ? 54 : 0) + 20;
    float center = r.x + r.w / 2.0f;
    if (line.kind == OverviewLine::Looking)
        drawSpinner(c, center, y - 50, 15);
    c.textCenter(_font, center, y, line.text, 20, t.text, true);
    for (auto& text : detail) {
        y += 22;
        c.textCenter(_font, center, y + 6, text, 15, t.dim);
    }
}

/// What's going on behind the overview, as on the lists. The buttons on the cards say what they do, so all that's down here is A for
/// whatever the pointer's on, and what goes for every screen.
void App::drawOverviewFooter(Canvas& c) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    c.fill(Rect{0, FooterY, ScreenW, 1}, t.line);
    constexpr float Line1 = 671, Line2 = 695;
    constexpr float Width = 440;
    JobKind kind = _jobKind;
    if (_analystRunning) {
        AnalysisShown a = analysisShownLocked();
        c.textFit(_font, Margin, Line1, (a.measuring ? "测量中 " : "正在分析 ") + a.title, 17, Width, t.text, true);
        drawBar(c, Rect{Margin, (int)Line2 - 10, 300, 5}, a.fraction, t.accent);
        c.text(_font, Margin + 312, Line2 - 3, format("%d of %d", a.index, a.count), 14.5f, t.dim);
    } else if (kind == JobKind::Starting || kind == JobKind::Scanning) {
        int tab = _scanningTab;
        c.text(_font, Margin, Line1, tab == TabFiles ? "正在查找游戏文件" : "正在查看已安装游戏", 17, t.text, true);
        if (kind == JobKind::Scanning && _jobCount > 0)
            c.text(_font, Margin, Line2, format("%d of %d", _jobIndex, _jobCount), 14.5f, t.dim);
    }

    const Overview& overview = overviewLocked();
    OverviewLayout layout = overviewLayout(overview);
    std::vector<Hint> hints;
    if (!layout.stops.empty()) {
        OverviewStop stop = layout.stops[(size_t)overviewStopIndex(overview, layout)];
        OverviewLine line = overview.cards[stop.tab].lines[(size_t)stop.line];
        hints.push_back({"A", "确定", [this, stop, line] { pressOverviewStop(stop, line); }});
    }
    hints.push_back({"+", "菜单", [this] { openMenu(); }});
    hints.push_back({"B", "退出", [this] { pressQuit(); }});
    drawHints(c, hints);
}

// What a list with nothing on it says instead, which depends on why there's nothing.
void App::drawEmptyList(Canvas& c) {
    std::string where = whereGameFilesAre();
    JobKind kind = _jobKind;
    bool scanning = !_scanned[_tab] || kind == JobKind::Starting || (kind == JobKind::Scanning && _scanningTab == _tab);
    int done = doneHiddenLocked(_tab);
    if (_tab == TabInstalled && !_console)
        drawEmpty(c, "无法读取主机上的游戏", _platform.consoleProblem(), false);
    else if (scanning && _tab == TabInstalled)
        drawEmpty(c, "正在查看已安装内容", "", true);
    else if (scanning)
        drawEmpty(c, "正在查找游戏文件", "正在浏览 " + where + " 中的 NSP、NSZ、XCI、XCZ 文件及已解包游戏的文件夹。", true);
    else if (_view[_tab] == View::Done)
        drawEmpty(c, "这里还没有已完成的项目", _settings.compress ? std::string("压缩完成的游戏会出现在这里。")
                                                                     : std::string("重打包完成的游戏会出现在这里，没有可并入更新补丁的游戏也会。"), false);
    else if (done > 0)
        drawEmpty(c, "这里的项目都已完成", format("已完成 %d 个游戏，可在列表顶部的「已完成」中查看。", done), false);
    else if (_tab == TabInstalled)
        drawEmpty(c, "没有已安装的游戏", _hidden.empty() ? "" : "被隐藏的游戏可用 + 键重新显示。", false);
    else
        drawEmpty(c, "此处没有游戏文件", where + " 上的 NSP、NSZ、XCI、XCZ 文件以及已解包游戏的文件夹会出现在这里。" +
                                               (_settings.filesFolder.empty() ? "+ 菜单中的「搜索位置」可选择范围，并说明哪些文件夹被排除。" : ""), false);
}

void App::drawFooter(Canvas& c, const std::vector<Row*>& rows) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    c.fill(Rect{0, FooterY, ScreenW, 1}, t.line);

    constexpr float Line1 = 671, Line2 = 695;
    constexpr float Width = 440;
    JobKind kind = _jobKind;
    // A list that hasn't come up yet may only be waiting for another one to give way. What's being analyzed for another list is that list's business.
    bool scanningThis = _scanningTab == _tab && kind == JobKind::Scanning;
    bool looking = kind == JobKind::Starting || scanningThis || !_scanned[_tab];
    // While first looks are taken in the middle of measuring, it's the measuring that's shown (analysisShownLocked).
    AnalysisShown a = analysisShownLocked();
    bool analyzing = _analystRunning && a.tab == _tab && !looking;
    // What's going on for another list, as there's otherwise no telling anything is.
    static const char* const ListNames[] = {"已安装游戏", "游戏文件"};
    bool scanningOther = kind == JobKind::Scanning && _scanningTab != _tab && _scanningTab >= 0 && _scanningTab < TabCount;
    std::string elsewhere;
    if (scanningOther)
        elsewhere = std::string("同时正在浏览 ") + ListNames[_scanningTab.load()];
    else if (_analystRunning && a.tab != _tab)
        elsewhere = (a.measuring ? "同时测量 " : "同时正在分析 ") + a.title;

    int ticked = 0, unknown = 0, notSure = 0;
    i64 saved = 0;
    bool atLeast = false;
    for (auto* row : rows) {
        if (!row->ticked || !row->blocked.empty())
            continue;
        ticked++;
        notSure += row->hasEstimate && row->notSure ? 1 : 0;
        if (row->hasEstimate) {
            saved += row->sureSaved();
            atLeast = atLeast || row->estimate.atLeast() || row->notSure;
        } else {
            unknown++;
        }
    }

    if (analyzing) {
        c.textFit(_font, Margin, Line1, (a.measuring ? "测量中 " : "正在分析 ") + a.title, 17, Width, t.text, true);
        drawBar(c, Rect{Margin, (int)Line2 - 10, 300, 5}, a.fraction, t.accent);
        c.text(_font, Margin + 312, Line2 - 3, format("%d of %d", a.index, a.count), 14.5f, t.dim);
    } else if (looking) {
        const char* what = !rows.empty() ? "正在更新列表" : _tab == TabInstalled ? "正在查看已安装游戏" : "正在查找游戏文件";
        c.text(_font, Margin, Line1, what, 17, t.text, true);
        std::string second = scanningThis && _jobCount > 0 ? format("%d of %d", _jobIndex, _jobCount) : "";
        // It can't start until the one before it is done.
        if (scanningOther)
            second = std::string("之后 ") + ListNames[_scanningTab.load()];
        else if (_analystRunning)
            second += (second.empty() ? "" : Dot) + std::string(a.measuring ? "同时测量 " : "同时正在分析 ") + a.title;
        c.textFit(_font, Margin, Line2, second, 14.5f, Width, t.dim);
    } else if (!rows.empty()) {
        // On Done with nothing ticked, what compressing them came to, rather than how many are ticked.
        int compressedCount = 0;
        i64 freedAll = 0;
        if (_view[_tab] == View::Done && ticked == 0) {
            for (auto* row : rows) {
                if (isCompressed(*row)) {
                    compressedCount++;
                    freedAll += std::max<i64>(row->freed, row->hasEstimate ? row->estimate.freedBytes : 0);
                }
            }
        }
        std::string first = compressedCount > 0
            ? format("已压缩 %d 个游戏", compressedCount) + (freedAll > 0 ? Dot + formatSize(freedAll) + "，共释放" : "")
            : format("已勾选 %d / %d", ticked, (int)rows.size());
        c.textFit(_font, Margin, Line1, first, 17, Width, t.text, true);
        std::string second;
        Color color = t.dim;
        bool frees = _tab == TabInstalled && replacesInstalled();
        if (ticked == 0) {
            second = "在游戏上按 A 键以勾选";
        } else if (saved > 0) {
            second = atLeast ? format(frees ? "至少释放 %s" : "至少小 %s", formatSize(saved).c_str())
                             : format(frees ? "释放 %s" : "小 %s", formatSize(saved).c_str());
            if (unknown > 0)
                second += "，还有更多待处理";
            color = t.accent;
        } else if (!_settings.compress) {
            second = "按原样重打包，不压缩";
        } else if (unknown > 0) {
            second = "尚未分析";
        } else if (notSure > 0) {
            second = "尚未确定，已勾选：无论节省多少都会压缩";
        } else {
            second = "没有收益";
        }
        if (!elsewhere.empty())
            second += (second.empty() ? "" : Dot) + elsewhere;
        c.textFit(_font, Margin, Line2, second, 14.5f, Width, color);
    } else if (!elsewhere.empty()) {
        c.textFit(_font, Margin, Line2, elsewhere, 14.5f, Width, t.dim);
    }

    std::vector<Hint> hints;
    if (!rows.empty()) {
        hints.push_back({"A", "勾选", [this] { toggleRow(_focus[_tab]); }});
        hints.push_back({"Y", tickActionLabel(nextTickActionLocked()), [this] { cycleTicks(); }});
        // Only once something is ticked, rather than greyed out: the line above says to tick games.
        if (ticked > 0)
            hints.push_back({"X", actionWord(), [this] { pressCompress(); }});
    }
    hints.push_back({"+", "菜单", [this] { openMenu(); }});
    if (scanningThis)
        hints.push_back({"B", "停止查找", [this] { stopLooking(); }});
    else
        hints.push_back({"B", "退出", [this] { pressQuit(); }});
    drawHints(c, hints);
}

// -------------------------------------------------------------------------------------------------------------------
// Other screens

void App::drawOptions(Canvas& c) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    c.fill(Rect{0, TopBarH, ScreenW, 1}, t.line);

    constexpr int RowHeight = 50;
    constexpr int SectionHeight = 36;
    constexpr int ListStart = TopBarH + 4;
    constexpr float LargestValue = 720;
    constexpr float DescriptionW = 860;
    // Room below the list for the longest description, all of it.
    const int descriptionLines = this->descriptionLines(_options, DescriptionW);
    const int footerY = FooterY - (descriptionLines - 2) * 21;
    const int ListEnd = footerY - 2;

    // Where everything goes, to keep what has the focus in sight.
    int height = 0, focusTop = 0;
    {
        std::string section;
        for (size_t i = 0; i < _options.size(); i++) {
            if (_options[i].section != section) {
                section = _options[i].section;
                height += SectionHeight;
            }
            if ((int)i == _optionFocus)
                focusTop = height;
            height += RowHeight;
        }
    }
    double view = ListEnd - ListStart;
    if (_lastOptionFocus != _optionFocus) {
        _lastOptionFocus = _optionFocus;
        // The heading above the first of a section comes into sight with it.
        if (focusTop - SectionHeight < _optionScroll)
            _optionScroll = std::max(focusTop - SectionHeight, 0);
        if (focusTop + RowHeight > _optionScroll + view)
            _optionScroll = focusTop + RowHeight - view;
    }
    _optionScroll = std::max(0.0, std::min(_optionScroll, std::max(0.0, height - view)));

    c.setClip(Rect{0, ListStart, ScreenW, ListEnd - ListStart});
    int y = ListStart - (int)glideTowards(_optionScrollShown, _optionScroll);
    {
        Rect selection{Margin - 12, y + (int)glide(1, focusTop) + 2, ScreenW - (Margin - 12) * 2, RowHeight - 4};
        c.fillRound(selection, 10, t.focus);
        c.strokeRound(selection, 10, 2, t.accent);
    }
    std::string section;
    for (size_t i = 0; i < _options.size(); i++) {
        OptionItem& item = _options[i];
        if (item.section != section) {
            section = item.section;
            c.text(_font, Margin + 8, y + 25.0f, section, 14.5f, t.accent, true);
            y += SectionHeight;
        }
        bool focused = (int)i == _optionFocus;
        Rect area{Margin - 12, y + 2, ScreenW - (Margin - 12) * 2, RowHeight - 4};
        if (y + RowHeight < ListStart || y > ListEnd) {
            y += RowHeight;
            continue;
        }
        c.text(_font, Margin + 8, y + 32.0f, item.label, 18.5f, t.text);

        std::string value = item.value();
        // Every value ends in the same place, the arrows round the one being changed going either side of it.
        float right = ScreenW - Margin - 34.0f;
        float valueWidth = std::min(_font.measure(value, 18.5f, focused), LargestValue);
        c.textFit(_font, right - valueWidth, y + 32.0f, value, 18.5f, LargestValue, focused ? t.accent : t.dim, focused);
        if (focused && !item.action)
            drawChangeArrows(c, right - valueWidth - 18, right + 18, y + RowHeight / 2.0f, t.accent);
        int index = (int)i;
        int top = std::max(area.y, ListStart), bottom = std::min(area.y + area.h, ListEnd);
        if (bottom > top) {
            Rect touchable{area.x, top, area.w, bottom - top};
            _hits.push_back(Hit{touchable, [this, index] {
                if (_optionFocus == index)
                    _options[(size_t)index].change(1);
                _optionFocus = index;
            }});
            if (!item.action) {
                Rect back = touchable;
                back.x = area.x + area.w * 3 / 4;
                back.w = area.w / 8;
                _hits.push_back(Hit{back, [this, index] {
                    if (_optionFocus == index)
                        _options[(size_t)index].change(-1);
                    _optionFocus = index;
                }});
            }
        }
        y += RowHeight;
    }
    c.clearClip();

    if (height > view) {
        int thumb = std::max(40, (int)(view * view / height));
        int thumbY = ListStart + (int)((view - thumb) * (_optionScroll / (height - view)));
        c.fillRound(Rect{ScreenW - 14, thumbY, 4, thumb}, 2, t.faint);
    }

    c.fill(Rect{0, footerY, ScreenW, 1}, t.line);
    drawExplanation(c, Margin, 666.0f - (FooterY - footerY), _options[(size_t)_optionFocus].help, 15, DescriptionW, 21, t.dim, descriptionLines);
    bool acts = _options[(size_t)_optionFocus].action;
    drawHints(c, {{acts ? "A" : "<>", acts ? "选择" : "更改", [this] { _options[(size_t)_optionFocus].change(1); }}, {"B", "返回", [this] { closeSettings(); }}});
}

/// Its own screen, titled as the folder browser is, with the storage still in sight at the top.
void App::drawSettings(Canvas& c) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    c.text(_font, Margin, 47, "Settings", 24, t.text, true);
    {
        sys::Lock lock(_mutex);
        if (_freeUsb >= 0) {
            drawStorage(c, 770, 140, "SD 卡", _freeSd, _totalSd, 0);
            drawStorage(c, 930, 140, "主机内存", _freeNand, _totalNand, 0);
            drawStorage(c, 1090, 140, "USB", _freeUsb, _totalUsb, 0);
        } else {
            drawStorage(c, 830, 190, "SD 卡", _freeSd, _totalSd, 0);
            drawStorage(c, 1050, 190, "主机内存", _freeNand, _totalNand, 0);
        }
    }
    drawOptions(c);
}

void App::drawChangeArrows(Canvas& c, float left, float right, float mid, Color ink) {
    c.line(left + 4, mid - 7, left - 3, mid, 2.2f, ink);
    c.line(left - 3, mid, left + 4, mid + 7, 2.2f, ink);
    c.line(right - 4, mid - 7, right + 3, mid, 2.2f, ink);
    c.line(right + 3, mid, right - 4, mid + 7, 2.2f, ink);
}

void App::drawOpening(Canvas& c) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    c.clearClip();
    c.clear(t.background);
    c.textCenter(_font, ScreenW / 2.0f, ScreenH / 2.0f - 6, "hacBrewPackLZ", 31, t.text, true);
    c.textCenter(_font, ScreenW / 2.0f, ScreenH / 2.0f + 30, "Opening\xE2\x80\xA6", 17, t.dim);
}

void App::drawWelcome(Canvas& c) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    constexpr int Left = 140, Width = ScreenW - Left * 2;
    constexpr int SectionH = 30;

    c.text(_font, Left, 70, "欢迎使用 hacBrewPackLZ", 31, t.text, true);
    float y = c.textWrapped(_font, Left, 110, "以 Switch 可直接读取的压缩格式存放游戏，占用更小、游玩相同。按 A 勾选游戏，按 X 开始；+ 键打开其他功能。", 17, Width, 25, t.dim);
    y = c.textWrapped(_font, Left, y + 4, "以此方式存放的游戏需要签名补丁才能启动，更新补丁也需在此整合进游戏。", 17, Width, 25, t.warn);

    // Where each one goes, with a heading wherever a section starts. The rows are as tall as there's room for, up to the usual 46, all the
    // same, so that Start stays clear of the line along the bottom.
    int top = (int)y + 4;
    int sections = 0;
    {
        std::string section;
        for (auto& item : _welcome) {
            if (!item.section.empty() && item.section != section)
                sections++;
            section = item.section;
        }
    }
    const int ItemH = std::max(38, std::min(46, (FooterY - 12 - 5 - top - sections * SectionH) / std::max((int)_welcome.size(), 1)));
    std::vector<int> tops;
    {
        int at = top;
        std::string section;
        for (int i = 0; i < (int)_welcome.size(); i++) {
            if (!_welcome[(size_t)i].section.empty() && _welcome[(size_t)i].section != section)
                at += SectionH;
            section = _welcome[(size_t)i].section;
            tops.push_back(at);
            at += ItemH;
        }
    }
    if (_welcomeFocus + 1 < (int)_welcome.size()) {
        Rect selection{Left - 16, (int)glide(2, tops[(size_t)_welcomeFocus]) + 3, Width + 32, ItemH - 6};
        c.fillRound(selection, 10, t.focus);
        c.strokeRound(selection, 10, 2, t.accent);
    } else {
        // The button shows it by itself. Coming back from it, the selection starts where it lands rather than
        // sliding out of the button, which isn't the same shape.
        glide(3, 0);
    }
    std::string section;
    for (int i = 0; i < (int)_welcome.size(); i++) {
        OptionItem& item = _welcome[(size_t)i];
        bool focused = i == _welcomeFocus;
        bool isStart = i + 1 == (int)_welcome.size();
        if (!item.section.empty() && item.section != section)
            c.text(_font, Left + 4, tops[(size_t)i] - 8.0f, item.section, 14.5f, t.accent, true);
        section = item.section;
        Rect area{Left - 16, tops[(size_t)i] + 3, Width + 32, ItemH - 6};
        if (isStart) {
            Rect button{(ScreenW - 300) / 2, area.y + 8, 300, area.h};
            c.fillRound(button, 12, focused ? t.accent : t.raised);
            c.textCenter(_font, ScreenW / 2.0f, button.y + button.h / 2.0f + 7, item.label, 19, focused ? t.onAccent : t.text, focused);
            _hits.push_back(Hit{button, [this] { finishWelcome(); }});
            continue;
        }
        float baseline = area.y + area.h / 2.0f + 7;
        c.text(_font, Left + 4, baseline, item.label, 18.5f, t.text);
        std::string value = item.value();
        constexpr float LargestValue = 560;
        float right = Left + Width - 30.0f;
        float valueWidth = std::min(_font.measure(value, 18.5f, focused), LargestValue);
        c.textFit(_font, right - valueWidth, baseline, value, 18.5f, LargestValue, focused ? t.accent : t.dim, focused);
        if (focused && !item.action)
            drawChangeArrows(c, right - valueWidth - 18, right + 18, area.y + area.h / 2.0f, t.accent);
        _hits.push_back(Hit{area, [this, i] {
            if (_welcomeFocus == i)
                _welcome[(size_t)i].change(1);
            _welcomeFocus = i;
        }});
    }

    // Room for the longest description, all of it, below what's to choose, as far as there's room.
    constexpr float DescriptionW = 800;
    int itemsBottom = tops.empty() ? 0 : tops.back() + ItemH + 14;
    int footerY = std::max(FooterY - (descriptionLines(_welcome, DescriptionW) - 2) * 21, std::min(itemsBottom, FooterY));
    c.fill(Rect{0, footerY, ScreenW, 1}, t.line);
    drawExplanation(c, Margin, 666.0f - (FooterY - footerY), _welcome[(size_t)_welcomeFocus].help, 15, DescriptionW, 21, t.dim, 2 + (FooterY - footerY) / 21);
    bool acts = _welcome[(size_t)_welcomeFocus].action;
    drawHints(c, {{acts ? "A" : "<>", acts ? "选择" : "更改", [this] { _welcome[(size_t)_welcomeFocus].change(1); }}, {"+", "开始", [this] { finishWelcome(); }}, {"B", "退出", [this] { pressQuit(); }}});
}

void App::drawBrowse(Canvas& c) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    Browser& browser = *_browser;
    c.textFit(_font, Margin, 40, browser.title, 24, ScreenW - Margin * 2, t.text, true);
    std::string where = browser.path.empty() ? "选择起始位置" : browser.path;
    c.textFit(_font, Margin, 64, where, 15.5f, ScreenW - Margin * 2, t.dim);
    c.fill(Rect{0, TopBarH, ScreenW, 1}, t.line);

    constexpr int EntryH = 56;
    constexpr int ListStart = TopBarH + 2;
    constexpr int ListEnd = FooterY - 2;
    int view = ListEnd - ListStart;
    int count = (int)browser.entries.size();
    double top = browser.focus * (double)EntryH;
    if (browser.focus != browser.lastFocus) {
        browser.lastFocus = browser.focus;
        if (top < browser.scroll)
            browser.scroll = top;
        if (top + EntryH > browser.scroll + view)
            browser.scroll = top + EntryH - view;
    }
    browser.scroll = std::max(0.0, std::min(browser.scroll, std::max(0.0, (double)count * EntryH - view)));

    if (count == 0) {
        drawEmpty(c, browser.path.empty() ? "无可选择项" : "此文件夹内无子文件夹", browser.path.empty() ? "" : "可以直接选择此文件夹。", false);
    } else {
        c.setClip(Rect{0, ListStart, ScreenW, view});
        int scrolled = (int)glideTowards(browser.scrollShown, browser.scroll);
        {
            Rect selection{20, ListStart + (int)glide(3, browser.focus * (double)EntryH) - scrolled + 3, ScreenW - 44, EntryH - 6};
            c.fillRound(selection, 10, t.focus);
            c.strokeRound(selection, 10, 2, t.accent);
        }
        for (int i = 0; i < count; i++) {
            int y = ListStart + i * EntryH - scrolled;
            if (y + EntryH < ListStart || y > ListEnd)
                continue;
            const Browser::Entry& entry = browser.entries[(size_t)i];
            // A folder, drawn simply.
            Rect tab{Margin + 2, y + 16, 12, 5};
            Rect body{Margin + 2, y + 20, 28, 19};
            c.fillRound(tab, 2, t.dim);
            c.fillRound(body, 3, entry.note.empty() ? t.dim : t.accent);
            c.textFit(_font, Margin + 44.0f, y + 35.0f, entry.name, 18.5f, 900, t.text);
            if (!entry.note.empty())
                c.textRight(_font, ScreenW - Margin, y + 35.0f, entry.note, 15, t.accent);
            int topEdge = std::max(y, ListStart), bottomEdge = std::min(y + EntryH, ListEnd);
            if (bottomEdge > topEdge) {
                _hits.push_back(Hit{Rect{0, topEdge, ScreenW, bottomEdge - topEdge}, [this, i] {
                    if (!_browser || i >= (int)_browser->entries.size())
                        return;
                    if (_browser->focus == i)
                        browseInto(i);
                    else
                        _browser->focus = i;
                }});
            }
        }
        c.clearClip();
    }

    c.fill(Rect{0, FooterY, ScreenW, 1}, t.line);
    std::vector<Hint> hints;
    if (count > 0) {
        hints.push_back({"A", "打开", [this] {
            if (_browser)
                browseInto(_browser->focus);
        }});
    }
    if (!browser.path.empty()) {
        hints.push_back({"X", "使用此文件夹", [this] {
            if (_browser)
                browseUse();
        }});
    }
    if (!browser.useDefaultLabel.empty()) {
        hints.push_back({"Y", browser.useDefaultLabel, [this] {
            if (_browser)
                browseUseDefault();
        }});
    }
    hints.push_back({"B", browser.path.empty() ? "取消" : "Up", [this] {
        if (_browser)
            browseUp();
    }});
    hints.push_back({"-", "关闭", [this] {
        if (_browser)
            closeBrowser();
    }});
    drawHints(c, hints);
}

void App::drawJob(Canvas& c) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    sys::Lock lock(_mutex);
    JobKind kind = _jobKind;
    c.text(_font, Margin, 47, kind == JobKind::Extracting ? "Extracting" : kind == JobKind::Checking ? "Checking" : kind == JobKind::Removing ? "Removing"
                            : kind == JobKind::Fixing ? "Fixing" : verbing(), 24, t.text, true);
    if (_jobCount > 1)
        c.textRight(_font, ScreenW - Margin, 47, format("第 %d / %d 个游戏", std::max(_jobIndex, 1), _jobCount), 17, t.dim);
    c.fill(Rect{0, TopBarH, ScreenW, 1}, t.line);

    // With more than one game, the list of them goes beside the one being done, and what they come to together goes below it.
    bool several = _jobCount > 1 && !_jobKeys.empty();
    constexpr int Top = TopBarH + 24, CardH = 300, Pad = 24;
    Rect card{Margin, Top, several ? 760 : ScreenW - Margin * 2, CardH};
    c.fillRound(card, 16, t.card);

    constexpr int IconBig = 128;
    int left = card.x + Pad + IconBig + 24;
    int width = card.right() - Pad - left;
    drawTile(c, _jobIcon, _jobTile, _jobTitle, card.x + Pad, card.y + Pad, IconBig, 16);
    c.textFit(_font, (float)left, card.y + 60.0f, _jobTitle, 26, (float)width, t.text, true);
    c.textFit(_font, (float)left, card.y + 88.0f, _jobDetail, 15, (float)width, t.dim);
    c.textFit(_font, (float)left, card.y + 128.0f, _jobLabel, 17, (float)width, t.text);

    bool hasNumbers = _jobTotal > 1024;
    double stepFraction = _jobTotal > 0 ? (double)_jobDone / (double)_jobTotal : 0;
    drawBar(c, Rect{left, card.y + 142, width, 8}, stepFraction, t.accent);
    std::string numbers;
    if (hasNumbers) {
        numbers = format("%s of %s", formatSize(_jobDone).c_str(), formatSize(_jobTotal).c_str());
        if (_jobRate > 0)
            numbers += format("   \xC2\xB7   %.1f MB/s", _jobRate / (1 << 20));
    }
    c.text(_font, (float)left, card.y + 174.0f, numbers, 15, t.dim);
    if (_jobTotal > 0)
        c.textRight(_font, (float)(left + width), card.y + 174.0f, format("%.0f%%", stepFraction * 100), 15, t.dim);

    // The steps a game goes through, along the bottom of its card.
    int steps = (int)_jobSteps.size();
    if (steps > 0) {
        float stepsY = card.y + 227.0f;
        constexpr float StepRadius = 11;
        float x0 = card.x + 70.0f, x1 = card.right() - 70.0f;
        float gap = steps > 1 ? (x1 - x0) / (steps - 1) : 0;
        for (int i = 0; i + 1 < steps; i++)
            c.line(x0 + i * gap + StepRadius + 6, stepsY, x0 + (i + 1) * gap - StepRadius - 6, stepsY, 2, i < _jobStep ? t.accent : t.line);
        for (int i = 0; i < steps; i++) {
            float x = x0 + i * gap;
            if (i < _jobStep) {
                drawTick(c, x, stepsY, StepRadius, true, true);
            } else if (i == _jobStep) {
                c.fillCircle(x, stepsY, StepRadius, t.accent.withAlpha(70));
                c.fillCircle(x, stepsY, StepRadius * 0.45f, t.accent);
            } else {
                drawTick(c, x, stepsY, StepRadius, false, true);
            }
            c.textCenter(_font, x, stepsY + 37, _jobSteps[(size_t)i], 15, i <= _jobStep ? t.text : t.faint);
        }
    }

    // All of it: how far, how long, and what it's freed so far.
    double secondsLeft = 0;
    double overall = overallLocked(&secondsLeft);
    double elapsed = sys::now() - _jobStarted;
    int statsY = card.bottom() + 54;
    if (several) {
        int y = card.bottom() + 52;
        c.text(_font, Margin, (float)y, "合计", 19, t.text, true);
        c.textRight(_font, (float)card.right(), (float)y, format("%.0f%%", overall * 100), 17, t.accent, true);
        drawBar(c, Rect{Margin, y + 16, card.w, 8}, overall, t.accent);
        statsY = y + 78;
    }
    struct Stat {
        const char* label;
        std::string value;
        bool good;
    };
    std::vector<Stat> stats;
    stats.push_back({"剩余时间", overall > 0.02 && elapsed > 20 ? roughTime(secondsLeft) : "正在估算", false});
    stats.push_back({"已完成", roughTime(elapsed), false});
    if (_jobFilesDone > 0)
        stats.push_back({"文件", _jobFilesTotal > 0 ? format("%lld / %lld", (long long)_jobFilesDone, (long long)_jobFilesTotal) : format("%lld 已完成", (long long)_jobFilesDone), false});
    if (kind == JobKind::Compressing && _jobFreed > 0)
        stats.push_back({"已释放", formatSize(_jobFreed), true});
    int statGap = several ? (stats.size() > 3 ? 190 : 240) : 300;
    int statX = Margin;
    for (auto& stat : stats) {
        c.text(_font, (float)statX, (float)statsY, stat.label, 15, t.dim);
        c.text(_font, (float)statX, statsY + 34.0f, stat.value, 24, stat.good ? t.accent : t.text, true);
        statX += statGap;
    }

    if (several) {
        // Every game this time: what each saved once it's done, the one being done, and the rest with what they are now.
        Rect list{card.right() + 20, Top, ScreenW - Margin - card.right() - 20, FooterY - 20 - Top};
        c.fillRound(list, 16, t.card);
        c.text(_font, (float)(list.x + Pad), list.y + 38.0f, "本次", 19, t.text, true);
        c.textRight(_font, (float)(list.right() - Pad), list.y + 38.0f, format("%d 个游戏", (int)_jobKeys.size()), 15, t.dim);
        constexpr int EntryH = 52, MoreH = 30;
        int first = list.y + 54;
        int room = (list.bottom() - 16 - first) / EntryH;
        int count = (int)_jobKeys.size();
        int current = std::max(0, std::min(_jobIndex - 1, count - 1));
        int fits = count > room ? std::max(room - 1, 1) : count;
        int start = std::max(0, std::min(current - 2, count - fits));
        for (int i = start; i < count && i < start + fits; i++) {
            const std::string& key = _jobKeys[(size_t)i];
            const ResultLine* done = nullptr;
            for (auto& result : _results)
                done = result.key == key ? &result : done;
            const Row* row = findLocked(key);
            int y = first + (i - start) * EntryH;
            Rect entry{list.x + 12, y, list.w - 24, EntryH - 4};
            bool now = !done && i == current;
            if (now)
                c.fillRound(entry, 10, t.focus);
            std::string name = done ? done->name : row ? row->name : key;
            drawTile(c, smallIcon(done ? done->icon : row ? row->icon : nullptr, 36), done ? done->tile : row ? row->tile : t.raised, name, entry.x + 12, y + 6, 36, 8);
            if (!done && !now)
                c.fillRound(Rect{entry.x + 12, y + 6, 36, 36}, 8, t.card.withAlpha(110));
            float nameX = entry.x + 62.0f, right = (float)(entry.right() - 12);
            if (done) {
                bool saved = done->state == RowState::Done && done->after > 0 && done->after < done->before;
                bool failed = done->state == RowState::Failed;
                std::string said = saved ? formatSize(done->before - done->after) : done->outcome;
                if (failed) {
                    c.fillCircle(right - 10, y + 24.0f, 10, t.bad);
                    c.line(right - 14, y + 20.0f, right - 6, y + 28.0f, 2, t.card);
                    c.line(right - 6, y + 20.0f, right - 14, y + 28.0f, 2, t.card);
                } else if (done->state == RowState::Done) {
                    drawTick(c, right - 10, y + 24.0f, 10, true, true);
                }
                float saidW = _font.measure(said, 16);
                c.textRight(_font, right - 30, y + 30.0f, said, 16, failed ? t.bad : saved ? t.accent : t.dim);
                c.textFit(_font, nameX, y + 30.0f, name, 16, right - 40 - saidW - nameX, t.text);
            } else if (now) {
                drawSpinner(c, right - 10, y + 24.0f, 9);
                c.textFit(_font, nameX, y + 30.0f, name, 16, right - 30 - nameX, t.text, true);
            } else {
                std::string size = row ? formatSize(row->now) : "";
                float sizeW = _font.measure(size, 16);
                c.textRight(_font, right, y + 30.0f, size, 16, t.faint);
                c.textFit(_font, nameX, y + 30.0f, name, 16, right - 16 - sizeW - nameX, t.dim);
            }
        }
        int after = count - (start + fits);
        if (after > 0)
            c.text(_font, list.x + 74.0f, first + fits * EntryH + MoreH - 8.0f, format("还有 %d 个", after), 14.5f, t.faint);
    }

    c.fill(Rect{0, FooterY, ScreenW, 1}, t.line);
    bool replacing = _jobSteps.size() > 2;
    c.textWrapped(_font, Margin, 686, replacing ? "新副本校验通过前会保留原文件；若中途断电，下次启动会自动修复。"
                                    : kind == JobKind::Fixing ? "只修改主机中各游戏的记录，文件本身不变。" : "原文件不变。", 15, 880, 21, t.dim, 2);

    std::vector<Hint> hints;
    if (kind == JobKind::Removing || kind == JobKind::Fixing)
        hints.clear();
    else if (stopAsked())
        hints.push_back({"B", "正在停止", [] {}, false});
    else if (_stopAfterGame)
        hints.push_back({"B", "处理完此游戏后停止", [this] { askToStop(); }});
    else
        hints.push_back({"B", "停止", [this] { askToStop(); }});
    drawHints(c, hints);
}

/// How it went: what's been freed, and where, beside how full each place is now, then every game, those that went wrong first and the
/// rest saving the most first.
void App::drawResults(Canvas& c) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    sys::Lock lock(_mutex);
    c.text(_font, Margin, 47, _resultsTitle, 24, t.text, true);
    c.fill(Rect{0, TopBarH, ScreenW, 1}, t.line);

    // Each place games go, now and when this started: the part in the accent colour is what's been freed.
    struct Place {
        const char* label;
        i64 before, now, total;
    };
    std::vector<Place> places;
    const Place all[] = {{"SD 卡", _freeBefore[0], _freeSd, _totalSd}, {"主机内存", _freeBefore[1], _freeNand, _totalNand}, {"USB 盘", _freeBefore[2], _freeUsb, _totalUsb}};
    bool anyFreed = false;
    for (auto& place : all) {
        if (place.before >= 0 && place.now >= 0 && place.total > 0) {
            places.push_back(place);
            anyFreed = anyFreed || place.now - place.before >= (1 << 20);
        }
    }
    if (_jobFreed <= 0)
        places.clear();

    // In line with the card under it, and the heading above.
    constexpr int Left = Margin, Right = ScreenW - Margin;
    int top;
    if (_jobFreed > 0) {
        c.text(_font, Left, TopBarH + 52.0f, formatSize(_jobFreed) + " 已释放", 30, t.text, true);
        if (anyFreed) {
            const char* key = "已释放";
            float keyW = _font.measure(key, 14);
            c.fillRound(Rect{(int)(Right - keyW) - 22, TopBarH + 39, 12, 12}, 3, t.accent);
            c.textRight(_font, (float)Right, TopBarH + 50.0f, key, 14, t.dim);
        }
        float detailEnd = drawExplanation(c, Left, TopBarH + 82.0f, _resultsDetail, 16, ScreenW - Left * 2.0f, 22, t.dim, 2);
        // What each place says beside its bar, worked out first so the bars can end where the widest begins.
        constexpr int BarX = 168, BarW = 832;
        std::vector<std::vector<std::pair<std::string, Color>>> said;
        std::vector<float> widths;
        for (auto& place : places) {
            i64 freed = std::max<i64>(place.now - place.before, 0);
            std::vector<std::pair<std::string, Color>> parts;
            if (freed > 0)
                parts = {{"压缩前 " + formatSize(place.before), t.dim}, {"  \xE2\x86\x92  ", t.faint}, {formatSize(place.now), t.accent}, {" 可用", t.dim}};
            else
                parts = {{formatSize(place.now), t.text}, {" 可用", t.dim}};
            float width = 0;
            for (auto& part : parts)
                width += _font.measure(part.first, 16, part.second.r == t.accent.r && part.second.g == t.accent.g);
            said.push_back(std::move(parts));
            widths.push_back(width);
        }
        float widest = widths.empty() ? 0 : *std::max_element(widths.begin(), widths.end());
        int barW = std::max(100, std::min(BarW, Right - MeterTextGap - (int)widest - 1 - BarX));
        int barY = (int)detailEnd + 1;
        for (size_t i = 0; i < places.size(); i++) {
            auto& place = places[i];
            float baseline = barY + 11.0f;
            c.text(_font, Left, baseline, place.label, 17, t.text);
            i64 freed = std::max<i64>(place.now - place.before, 0);
            drawMeterBar(c, Rect{BarX, barY, barW, 12}, place.before, place.total, freed);
            float x = Right - widths[i];
            if (x < BarX + barW + MeterTextGap)
                _crowded.push_back(std::string(place.label) + "'s sizes");
            for (auto& part : said[i])
                x = c.text(_font, x, baseline, part.first, 16, part.second, part.second.r == t.accent.r && part.second.g == t.accent.g);
            barY += 40;
        }
        top = places.empty() ? TopBarH + 112 : barY - 40 + 12 + 35;
    } else {
        // Nothing was freed to show: what happened, said in full, over as many lines as it takes.
        auto lines = wrapText(_font, _resultsDetail, 21, ScreenW - Left * 2.0f);
        if (lines.size() > 3) {
            _cut.push_back(_resultsDetail);
            lines.resize(3);
        }
        float y = TopBarH + 52.0f;
        for (auto& line : lines) {
            c.text(_font, Left, y, line, 21, t.text, true);
            y += 28;
        }
        top = (int)y + 4;
    }

    Rect card{Margin, top, ScreenW - Margin * 2, FooterY - 20 - top};
    int count = (int)_results.size();
    if (count == 0) {
        drawEmpty(c, "未做任何改动", _resultsAdvice, false);
    } else {
        c.fillRound(card, 16, t.card);
        int done = 0, failed = 0;
        for (auto& result : _results) {
            done += result.state == RowState::Done ? 1 : 0;
            failed += result.state == RowState::Failed ? 1 : 0;
        }
        std::string heading = failed > 0 ? format("完成 %d 个，失败 %d 个", done, failed) : format("已完成 %d 个", done);
        c.text(_font, (float)(card.x + 24), card.y + 38.0f, heading, 19, t.text, true);
        // The order only goes by saving when something saved: fixing, removing, checking or extracting doesn't.
        bool anySaved = std::any_of(_results.begin(), _results.end(), [](const ResultLine& r) { return r.state == RowState::Done && r.after > 0 && r.after < r.before; });
        const char* sortedBy = failed > 0 ? "先显示出错的" : anySaved ? "先显示节省最多的" : "";
        if (*sortedBy)
            c.textRight(_font, (float)(card.right() - 24), card.y + 38.0f, sortedBy, 15, t.dim);

        // What went wrong first, then the biggest saving, then the rest as they came.
        std::vector<int> order((size_t)count);
        for (int i = 0; i < count; i++)
            order[(size_t)i] = i;
        auto rank = [&](const ResultLine& r) { return r.state == RowState::Failed ? 0 : r.state == RowState::Done ? 1 : 2; };
        auto saving = [&](const ResultLine& r) { return r.state == RowState::Done && r.after > 0 ? r.before - r.after : 0; };
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            const ResultLine& x = _results[(size_t)a];
            const ResultLine& y = _results[(size_t)b];
            if (rank(x) != rank(y))
                return rank(x) < rank(y);
            return saving(x) > saving(y);
        });

        // What went wrong with a game is said in full, over as many as three lines, so its row is as tall as that takes.
        constexpr int ResultH = 64;
        constexpr float RightW = 340;
        constexpr int MessageLine = 19;
        float messageW = card.right() - 24 - RightW - (card.x + 92.0f) + RightW - 140;
        auto messageLines = [&](const ResultLine& r) { return r.message.empty() ? 1 : std::min(3, (int)wrapText(_font, r.message, 14.5f, messageW).size()); };
        auto heightOf = [&](const ResultLine& r) { return ResultH + (messageLines(r) - 1) * MessageLine; };
        int first = card.y + 54;
        int room = card.bottom() - 12 - first;
        // As many from the top as fit, and the top no further down than leaves the last ones filling the card.
        auto fitting = [&](int from) {
            int shown = 0, used = 0;
            for (int k = from; k < count; k++) {
                used += heightOf(_results[(size_t)order[(size_t)k]]);
                if (used > room && shown > 0)
                    break;
                shown++;
            }
            return shown;
        };
        int lastTop = count - 1;
        for (int used = 0; lastTop > 0; lastTop--) {
            used += heightOf(_results[(size_t)order[(size_t)lastTop]]);
            if (used > room) {
                lastTop++;
                break;
            }
        }
        _resultTop = std::max(0, std::min(_resultTop, std::max(lastTop, 0)));
        _resultsShown = std::max(1, fitting(_resultTop));
        int y = first;
        for (int k = _resultTop; k < count && k < _resultTop + _resultsShown; k++) {
            const ResultLine& result = _results[(size_t)order[(size_t)k]];
            int rowY = y;
            y += heightOf(result);
            int y = rowY;
            drawTile(c, smallIcon(result.icon, 44), result.tile, result.name, card.x + 32, y + 8, 44, 9);
            float textX = card.x + 92.0f;
            float textW = card.right() - 24 - RightW - textX;
            bool failed = result.state == RowState::Failed;
            c.textFit(_font, textX, y + 26.0f, result.name, 18, textW + (failed ? RightW - 140 : 0), t.text);
            i64 saved = saving(result);
            if (!result.message.empty()) {
                drawExplanation(c, textX, y + 48.0f, result.message, 14.5f, messageW, MessageLine, failed ? t.bad : t.warn, 3);
            } else if (result.after > 0) {
                c.textFit(_font, textX, y + 48.0f, formatSize(result.before) + "  \xE2\x86\x92  " + formatSize(result.after), 14.5f, textW, t.dim);
            }
            if (saved > 0) {
                // What it came to, as a bar like the lists have, and what that saved.
                drawShareBar(c, Rect{card.right() - 24 - 290, y + 26, 160, 6}, (double)(result.before - saved) / (double)std::max<i64>(result.before, 1));
                c.textRight(_font, (float)(card.right() - 24), y + 36.0f, formatSize(saved), 17, t.accent, true);
            } else {
                Color color = failed ? t.bad : result.state == RowState::Done ? t.accent : t.dim;
                c.textRight(_font, (float)(card.right() - 24), y + 36.0f, result.outcome, 17, color, true);
            }
        }
        if (count > _resultsShown) {
            int span = room;
            int thumb = std::max(40, span * _resultsShown / count);
            int thumbY = first + (span - thumb) * _resultTop / std::max(count - _resultsShown, 1);
            c.fillRound(Rect{card.right() - 10, thumbY, 4, thumb}, 2, t.faint);
        }
    }

    c.fill(Rect{0, FooterY, ScreenW, 1}, t.line);
    bool anyFailed = std::any_of(_results.begin(), _results.end(), [](const ResultLine& r) { return r.state == RowState::Failed; });
    if (anyFailed)
        c.textFit(_font, Margin, 686, _log ? "错误详情也记录在 " + _logPath : std::string("如需保存错误日志，请在「更多设置」中开启"), 15, 900, t.dim);
    // Not "Done", which is a view on the list too, where what this compressed now is.
    drawHints(c, {{"+", "菜单", [this] { openMenu(MenuResults); }}, {"B", "返回列表", [this] { _screen = Screen::Main; }}});
}

void App::drawNoKeys(Canvas& c) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    c.text(_font, Margin, 46, "hacBrewPackLZ", 23, t.text, true);
    c.fill(Rect{0, TopBarH, ScreenW, 1}, t.line);

    constexpr float Left = 240;
    constexpr float Width = 800;
    c.text(_font, Left, 190, "此操作需要主机密钥", 29, t.text, true);
    float y = c.textWrapped(_font, Left, 236, "密钥用于解密游戏，只能从你自己的主机导出，本程序不附带。获取大约需要一分钟：", 18, Width, 27, t.dim);

    const char* steps[] = {"重启进入 payload 引导器，运行 Lockpick_RCM。", "选择导出主机密钥，密钥会保存到 SD 卡。", "回到此处并按 A 键。"};
    y += 16;
    int number = 1;
    for (const char* step : steps) {
        c.fillCircle(Left + 14, y - 6, 14, t.raised);
        c.textCenter(_font, Left + 14, y, format("%d", number++), 15, t.text, true);
        c.textFit(_font, Left + 44, y, step, 18, Width - 44, t.text);
        y += 44;
    }
    y += 6;
    auto where = _platform.keyLocations();
    c.textFit(_font, Left, y, "已查找 " + (where.empty() ? std::string() : where[0]), 15, Width, t.faint);
    if (!_keysProblem.empty())
        c.textWrapped(_font, Left, y + 26, _keysProblem, 15, Width, 21, t.warn, 3);

    c.fill(Rect{0, FooterY, ScreenW, 1}, t.line);
    drawHints(c, {{"A", "重试", [this] { lookForKeysAgain(); }}, {"+", "退出", [this] { pressQuit(); }}});
}

void App::drawDialog(Canvas& c) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    // Only what's on top can be touched.
    _hits.clear();
    double risen = _dialogSince >= 0 ? easeOut((_time - _dialogSince) / RiseTime) : 1;
    if (risen < 1)
        _dirty = true;

    Dialog& dialog = *_dialog;
    // A web address goes beside the words as a QR code, white whatever the theme, as that's what phones read best.
    constexpr int QrBox = 210, QrGap = 28;
    if (!dialog.qr.empty() && dialog.qrSize == 0) {
        std::vector<u8> temp(qrcodegen_BUFFER_LEN_MAX), code(qrcodegen_BUFFER_LEN_MAX);
        if (qrcodegen_encodeText(dialog.qr.c_str(), temp.data(), code.data(), qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
            dialog.qrSize = qrcodegen_getSize(code.data());
            dialog.qrModules.resize((size_t)(dialog.qrSize * dialog.qrSize));
            for (int y = 0; y < dialog.qrSize; y++) {
                for (int x = 0; x < dialog.qrSize; x++)
                    dialog.qrModules[(size_t)(y * dialog.qrSize + x)] = qrcodegen_getModule(code.data(), x, y) ? 1 : 0;
            }
        } else {
            dialog.qrSize = -1;
        }
    }
    bool qr = dialog.qrSize > 0;
    const int Width = qr ? 680 + QrBox + QrGap : 680;
    constexpr int Pad = 36;
    constexpr float TextSize = 17, LineH = 25, ParagraphGap = 11;
    constexpr int ButtonH = 50;
    int inner = Width - Pad * 2;
    int textWidth = qr ? inner - QrBox - QrGap : inner;

    std::vector<std::vector<std::string>> paragraphs;
    int height = Pad + 34 + 12;
    int textHeight = 0;
    for (auto& paragraph : dialog.paragraphs) {
        paragraphs.push_back(wrapText(_font, paragraph, TextSize, (float)textWidth));
        textHeight += (int)(paragraphs.back().size() * LineH + ParagraphGap);
    }
    height += qr ? std::max(textHeight, QrBox + 30) : textHeight;
    height += 16 + ButtonH + Pad - 8;

    Rect card{(ScreenW - Width) / 2, std::max((ScreenH - height) / 2, 8) + (int)((1 - risen) * 24), Width, height};
    c.shadow(card, 18, 26, Color{0, 0, 0, 90});
    c.fillRound(card, 18, t.card);

    c.textFit(_font, (float)card.x + Pad, card.y + Pad + 22.0f, dialog.title, 24, (float)inner, t.text, true);
    float y = card.y + Pad + 34 + 12 + 17.0f;
    for (auto& lines : paragraphs) {
        for (auto& line : lines) {
            c.text(_font, (float)card.x + Pad, y, line, TextSize, t.dim);
            y += LineH;
        }
        y += ParagraphGap;
    }
    if (qr) {
        Rect box{card.right() - Pad - QrBox, card.y + Pad + 34 + 12, QrBox, QrBox};
        c.fillRound(box, 10, Color{255, 255, 255, 255});
        // Four modules of white all round, as phones need.
        int module = QrBox / (dialog.qrSize + 8);
        int start = (QrBox - module * dialog.qrSize) / 2;
        for (int my = 0; my < dialog.qrSize; my++) {
            for (int mx = 0; mx < dialog.qrSize; mx++) {
                if (dialog.qrModules[(size_t)(my * dialog.qrSize + mx)])
                    c.fill(Rect{box.x + start + mx * module, box.y + start + my * module, module, module}, Color{0, 0, 0, 255});
            }
        }
        if (!dialog.qrCaption.empty())
            c.textCenter(_font, box.x + QrBox / 2.0f, (float)box.bottom() + 24, dialog.qrCaption, 15, t.dim, false);
    }

    int count = (int)dialog.choices.size();
    constexpr int Gap = 12;
    int buttonW = (inner - Gap * (count - 1)) / count;
    int buttonY = card.bottom() - Pad + 8 - ButtonH;
    for (int i = 0; i < count; i++) {
        Rect button{card.x + Pad + i * (buttonW + Gap), buttonY, buttonW, ButtonH};
        bool focused = i == dialog.focus;
        c.fillRound(button, 12, focused ? t.accent : t.raised);
        c.textCenter(_font, button.x + button.w / 2.0f, button.y + 32.0f, dialog.choices[(size_t)i].label, 18, focused ? t.onAccent : t.text, focused);
        _hits.push_back(Hit{button, [this, i] {
            auto action = _dialog->choices[(size_t)i].action;
            _dialog.reset();
            if (action)
                action();
        }});
    }
}

void App::drawMenuBehind(Canvas& c) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    c.fill(Rect{0, 0, ScreenW, ScreenH}, t.scrim.withAlpha(150));
    constexpr int Width = 520, ItemH = 50, Pad = 10;
    int height = (int)_menu->items.size() * ItemH + Pad * 2;
    c.shadow(Rect{(ScreenW - Width) / 2, (ScreenH - height) / 2, Width, height}, 16, 22, Color{0, 0, 0, 90});
}

void App::drawMenu(Canvas& c) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    _hits.clear();
    _hits.push_back(Hit{Rect{0, 0, ScreenW, ScreenH}, [this] { _menu.reset(); }});

    Menu& menu = *_menu;
    constexpr int Width = 520, ItemH = 50, Pad = 10;
    int height = (int)menu.items.size() * ItemH + Pad * 2;
    double risen = _menuSince >= 0 ? easeOut((_time - _menuSince) / RiseTime) : 1;
    if (risen < 1)
        _dirty = true;
    Rect card{(ScreenW - Width) / 2, (ScreenH - height) / 2 + (int)((1 - risen) * 24), Width, height};
    c.fillRound(card, 16, t.card);
    {
        Rect selection{card.x + Pad, card.y + Pad + (int)glide(10 + menu.page, menu.focus * ItemH), Width - Pad * 2, ItemH};
        c.fillRound(selection, 10, t.focus);
        c.strokeRound(selection, 10, 2, t.accent);
    }
    for (int i = 0; i < (int)menu.items.size(); i++) {
        const Menu::Item& entry = menu.items[(size_t)i];
        Rect item{card.x + Pad, card.y + Pad + i * ItemH, Width - Pad * 2, ItemH};
        bool focused = i == menu.focus;
        c.textFit(_font, item.x + 16.0f, item.y + 32.0f, entry.label, 18, item.w - 32.0f, t.text);
        if (entry.value) {
            // What it's set to, with left and right to change it.
            std::string value = entry.value();
            constexpr float ArrowGap = 16;
            float right = (float)item.x + item.w - 30;
            float valueWidth = _font.measure(value, 18, focused);
            Color ink = focused ? t.accent : t.dim;
            c.textRight(_font, right, item.y + 32.0f, value, 18, ink, focused);
            drawChangeArrows(c, right - valueWidth - ArrowGap, right + ArrowGap, item.y + ItemH / 2.0f, ink);
        }
        _hits.push_back(Hit{item, [this, i] {
            Menu::Item& tapped = _menu->items[(size_t)i];
            _menu->focus = i;
            if (tapped.change) {
                tapped.change(1);
                return;
            }
            auto action = tapped.action;
            _menu.reset();
            if (action)
                action();
        }});
    }
}

// -------------------------------------------------------------------------------------------------------------------
// Details and Look inside

namespace {

/// Each kind of file in a colour of its own, which is what lets a long folder be read at a glance: its tile's, and its mark's.
struct KindLook {
    Color back, ink;
};

KindLook kindLook(const std::string& kind, bool dark) {
    struct Look {
        const char* kind;
        Color darkBack, darkInk, lightBack, lightInk;
    };
    static const Look Looks[] = {
        {"Folder", {58, 74, 92}, {207, 224, 242}, {221, 232, 245}, {44, 92, 150}},
        {"Text", {60, 74, 63}, {205, 232, 210}, {222, 240, 226}, {40, 120, 62}},
        {"Picture", {79, 63, 92}, {229, 211, 242}, {236, 226, 245}, {110, 64, 150}},
        {"Texture", {79, 63, 92}, {229, 211, 242}, {236, 226, 245}, {110, 64, 150}},
        {"Sound", {92, 74, 51}, {242, 224, 196}, {247, 236, 218}, {150, 96, 20}},
        {"Video", {92, 58, 66}, {242, 208, 216}, {247, 224, 228}, {160, 50, 70}},
        {"Archive", {77, 59, 59}, {242, 208, 208}, {245, 226, 226}, {150, 60, 60}},
        {"Font", {52, 80, 80}, {200, 240, 236}, {220, 242, 240}, {20, 120, 110}},
        {"Model", {66, 66, 92}, {214, 214, 246}, {228, 228, 247}, {70, 70, 160}},
        {"Program", {74, 70, 52}, {240, 232, 196}, {244, 240, 220}, {120, 100, 20}},
    };
    for (auto& look : Looks) {
        if (kind == look.kind)
            return dark ? KindLook{look.darkBack, look.darkInk} : KindLook{look.lightBack, look.lightInk};
    }
    return dark ? KindLook{{61, 64, 68}, {201, 205, 210}} : KindLook{{232, 234, 237}, {90, 96, 104}};
}

/// The mark for a kind, drawn on a 24-unit grid into a tile, at three quarters of the tile's size.
void drawKindMark(Canvas& c, const std::string& kind, int x, int y, int tile, Color ink) {
    float scale = tile * 0.6f / 24, inset = tile * 0.2f;
    float thick = std::max(1.5f, tile / 17.0f);
    auto path = [&](std::initializer_list<std::pair<float, float>> points, bool closed) {
        std::vector<std::pair<float, float>> at;
        for (auto& p : points)
            at.emplace_back(x + inset + p.first * scale, y + inset + p.second * scale);
        for (size_t i = 0; i + 1 < at.size(); i++)
            c.line(at[i].first, at[i].second, at[i + 1].first, at[i + 1].second, thick, ink);
        if (closed && at.size() > 2)
            c.line(at.back().first, at.back().second, at[0].first, at[0].second, thick, ink);
    };
    if (kind == "Folder") {
        path({{3, 6}, {9, 6}, {11, 8}, {21, 8}, {21, 19}, {3, 19}}, true);
    } else if (kind == "Text") {
        path({{6, 3}, {15, 3}, {19, 7}, {19, 21}, {6, 21}}, true);
        path({{9, 12}, {16, 12}}, false);
        path({{9, 16}, {16, 16}}, false);
    } else if (kind == "Picture" || kind == "Texture") {
        path({{4, 4}, {20, 4}, {20, 20}, {4, 20}}, true);
        path({{4, 16}, {9, 11}, {13, 15}, {16, 12}, {20, 16}}, false);
    } else if (kind == "Sound") {
        path({{4, 10}, {8, 10}, {13, 6}, {13, 18}, {8, 14}, {4, 14}}, true);
        path({{16, 9}, {17.5f, 12}, {16, 15}}, false);
    } else if (kind == "Video") {
        path({{3, 6}, {16, 6}, {16, 18}, {3, 18}}, true);
        path({{16, 10}, {21, 7}, {21, 17}, {16, 14}}, false);
    } else if (kind == "Archive") {
        path({{4, 7}, {12, 3}, {20, 7}, {20, 17}, {12, 21}, {4, 17}}, true);
        path({{4, 7}, {12, 11}, {20, 7}}, false);
        path({{12, 11}, {12, 21}}, false);
    } else if (kind == "Font") {
        path({{6, 20}, {12, 4}, {18, 20}}, false);
        path({{8.5f, 14}, {15.5f, 14}}, false);
    } else if (kind == "Model") {
        path({{12, 3}, {21, 12}, {12, 21}, {3, 12}}, true);
    } else if (kind == "Program") {
        path({{8, 8}, {4, 12}, {8, 16}}, false);
        path({{16, 8}, {20, 12}, {16, 16}}, false);
        path({{13.5f, 6}, {10.5f, 18}}, false);
    } else {
        path({{6, 3}, {15, 3}, {19, 7}, {19, 21}, {6, 21}}, true);
    }
}

void drawKindTile(Canvas& c, const std::string& kind, int x, int y, int size, bool dark) {
    KindLook look = kindLook(kind, dark);
    c.fillRound(Rect{x, y, size, size}, size / 4, look.back);
    drawKindMark(c, kind, x, y, size, look.ink);
}

/// The game's icon, small, or its letters on its colour when it has none.
void drawGameMark(Canvas& c, Font& font, const std::shared_ptr<Image>& icon, Color tile, const std::string& name, Rect r, int radius) {
    if (icon && icon->w == r.w && icon->h == r.h) {
        c.image(*icon, r.x, r.y, radius);
        return;
    }
    c.fillRound(r, radius, tile);
    float size = r.w * 0.38f;
    c.textCenter(font, r.x + r.w / 2.0f, r.y + r.h / 2.0f + size * 0.36f, initialsOf(name), size, Color{255, 255, 255}, true);
}

/// A ">" between the places on the way back.
void drawChevron(Canvas& c, float x, float mid, Color color) {
    c.line(x, mid - 5, x + 5, mid, 1.8f, color);
    c.line(x + 5, mid, x, mid + 5, 1.8f, color);
}

/// Text with every character the same width, which code and bytes need to line up: drawn a character at a time, as the font's aren't.
/// Wide characters, as Chinese, Japanese and Korean ones are, take two. Whatever goes past the right is left out.
void monoText(Canvas& c, Font& font, float x, float baseline, const std::string& text, float size, float cell, Color color, float right) {
    for (size_t i = 0; i < text.size() && x + cell <= right + 0.5f;) {
        size_t next = nextCharacter(text, i);
        unsigned char lead = (unsigned char)text[i];
        float width = cell * (lead >= 0xE3 && lead <= 0xEF ? 2 : 1);
        // Each in the middle of its place, so a narrow letter doesn't leave a gap after it, and a wide one a little smaller, so it doesn't
        // run into the next.
        if (text[i] != ' ') {
            std::string one = text.substr(i, next - i);
            float glyph = size, w = font.measure(one, size);
            if (w > width - 1) {
                glyph = std::max(size * 0.72f, size * (width - 1) / w);
                w = font.measure(one, glyph);
            }
            c.text(font, x + (width - w) / 2, baseline, one, glyph, color);
        }
        x += width;
        i = next;
    }
}

/// How wide each character's place is, in Inconsolata, where every character is as wide as the others.
float monoCell(Font& mono, float size) { return mono.measure("0", size); }

/// Behind a picture, so what's see-through in it shows as that.
void drawCheckerboard(Canvas& c, Rect r, Color a, Color b) {
    constexpr int Square = 10;
    for (int y = r.y; y < r.bottom(); y += Square) {
        for (int x = r.x; x < r.right(); x += Square)
            c.fill(Rect{x, y, std::min(Square, r.right() - x), std::min(Square, r.bottom() - y)}, (((x - r.x) / Square + (y - r.y) / Square) & 1) ? a : b);
    }
}

/// A line of bytes as they are: where it starts, each byte, and what they'd read as.
void hexLine(const Bytes& raw, size_t at, size_t per, std::string& where, std::string& bytes, std::string& shown) {
    where = format("%08X", (unsigned)at);
    bytes.clear();
    shown.clear();
    for (size_t i = 0; i < per; i++) {
        if (at + i < raw.size()) {
            bytes += format("%02X ", raw[at + i]);
            u8 c = raw[at + i];
            shown += c >= 0x20 && c < 0x7F ? (char)c : '.';
        } else {
            bytes += "   ";
        }
        if (i + 1 == per / 2)
            bytes += ' ';
    }
}

/// How a kind is called when there are several of it.
std::string kindPlural(const std::string& kind, int count) {
    static const std::pair<const char*, const char*> Words[] = {
        {"Text", "文本文件"}, {"Picture", "图片"}, {"Texture", "纹理"}, {"Sound", "音频"}, {"Video", "视频"},
        {"Archive", "压缩包"}, {"Font", "字体"}, {"Model", "模型"}, {"Program", "程序文件"}, {"Data", "其他文件"},
    };
    for (auto& word : Words) {
        if (kind == word.first)
            return count == 1 && kind != "Data" ? (kind == "Text" ? std::string("文本文件") : kind == "Program" ? std::string("程序文件") : std::string(word.second))
                                                : std::string(word.second);
    }
    return kind;
}

}  // namespace

void App::drawDetails(Canvas& c) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    sys::Lock lock(_mutex);
    if (!_inside)
        return;
    Inside& in = *_inside;
    static const char* const ListNames[] = {"已安装游戏", "游戏文件"};
    constexpr int Top = 96, Bottom = FooterY - 16;

    // The way back: B, and where it was opened from.
    drawButtonGlyph(c, Margin + 13.0f, 36, "B", 13, t.raised, t.dim);
    float x = c.text(_font, Margin + 38.0f, 43, ListNames[std::min(std::max(in.tab, 0), 1)], 19, t.dim);
    drawChevron(c, x + 12, 37, t.faint);
    c.text(_font, x + 29, 43, "详细信息", 19, t.text);
    c.fill(Rect{0, TopBarH, ScreenW, 1}, t.line);

    // On the left, one card as tall as the facts beside it: the game's icon on its own colour, its name and publisher, and at the bottom the
    // one figure that decides. Nothing in it is cut short: what's long wraps, and a figure too wide gets smaller.
    constexpr int LeftW = 360, Inner = LeftW - 40;
    Rect left{Margin, Top, LeftW, Bottom - Top};
    c.fillRound(left, 16, t.card);
    Rect art{left.x, left.y, left.w, 224};
    Color tint = mixColor(t.card, in.tile, _dark ? 130 : 95);
    c.fillRound(art, 16, tint);
    c.fill(Rect{art.x, art.bottom() - 16, art.w, 16}, tint);
    Rect icon{art.x + (art.w - 176) / 2, art.y + 24, 176, 176};
    c.shadow(icon, 14, 16, Color{0, 0, 0, 90});
    drawGameMark(c, _font, in.iconBig, in.tile, in.name, icon, 14);
    float baseline = art.bottom() + 40.0f;
    float nameSize = 22;
    auto nameLines = wrapText(_font, in.name, nameSize, Inner);
    if (nameLines.size() > 3) {
        nameSize = 18.5f;
        nameLines = wrapText(_font, in.name, nameSize, Inner);
    }
    for (size_t i = 0; i < nameLines.size() && i < 3; i++) {
        std::string line = nameLines[i];
        if (i == 2 && nameLines.size() > 3)
            line = fitText(_font, line + " " + nameLines[3], nameSize, Inner, true);
        c.text(_font, left.x + 20.0f, baseline, line, nameSize, t.text, true);
        baseline += nameSize * 1.3f;
    }
    if (!in.publisher.empty()) {
        auto lines = wrapText(_font, in.publisher, 15.5f, Inner);
        for (size_t i = 0; i < lines.size() && i < 2; i++) {
            c.text(_font, left.x + 20.0f, baseline + 1, lines[i], 15.5f, t.dim);
            baseline += 21;
        }
    }

    if (in.ready) {
        // From the bottom up: the bar, the line under the figure, the figure, and what it is.
        bool bar = in.share >= 0;
        auto label = wrapText(_font, in.stateLabel, 15.5f, Inner);
        auto sub = in.stateSub.empty() ? std::vector<std::string>() : wrapText(_font, in.stateSub, 15, Inner);
        float bigSize = 30;
        while (bigSize > 20 && _font.measure(in.stateBig, bigSize, true) > Inner)
            bigSize -= 1;
        float height = 20 + label.size() * 21.0f + bigSize * 1.25f + sub.size() * 20.0f + (bar ? 34 : 0) + 18;
        float y = left.bottom() - height;
        c.fill(Rect{left.x + 20, (int)y, Inner, 1}, t.line);
        y += 20;
        for (auto& line : label) {
            y += 21;
            c.text(_font, left.x + 20.0f, y - 5, line, 15.5f, t.dim);
        }
        y += bigSize * 1.25f;
        c.text(_font, left.x + 20.0f, y - bigSize * 0.25f, fitText(_font, in.stateBig, bigSize, Inner, true), bigSize, bar ? t.accent : t.text, true);
        for (auto& line : sub) {
            y += 20;
            c.text(_font, left.x + 20.0f, y - 3, line, 15, t.dim);
        }
        if (bar) {
            // What it comes to, beside a bar of what it is now.
            double share = std::min(std::max(in.share, 0.0), 1.0);
            std::string word = format("%s %.0f%%", in.shareWord.c_str(), (1 - share) * 100);
            float wordW = _font.measure(word, 15.5f);
            Rect track{left.x + 20, (int)y + 16, (int)(Inner - wordW - 14), 10};
            drawShareBar(c, track, share);
            c.text(_font, track.right() + 14.0f, track.y + 10.0f, word, 15.5f, t.dim);
        }
    }

    // On the right, the facts, in four places that are always the same, then what's good to know about it.
    Rect panel{Margin + LeftW + 32, Top, ScreenW - Margin - (Margin + LeftW + 32), Bottom - Top};
    c.fillRound(panel, 16, t.card);
    if (!in.ready) {
        drawSpinner(c, panel.x + panel.w / 2.0f, panel.y + panel.h / 2.0f - 20, 15);
        c.textCenter(_font, panel.x + panel.w / 2.0f, panel.y + panel.h / 2.0f + 28, "正在打开", 18, t.dim);
    } else {
        constexpr int Pad = 28, Gap = 36;
        const int colW = (panel.w - Pad * 2 - Gap) / 2;
        int labelW = 100;
        for (auto& section : in.sections) {
            for (auto& line : section.lines)
                labelW = std::max(labelW, (int)_font.measure(line.label, 16.5f) + 18);
        }
        const int LabelW = std::min(labelW, 150);
        const int valueW = colW - LabelW;
        auto lineHeight = [&](const DetailsLine& line, std::vector<std::string>& value, std::vector<std::string>& note) {
            value = wrapText(_font, line.value, 17, (float)valueW);
            note = line.note.empty() ? std::vector<std::string>() : wrapText(_font, line.note, 14, (float)valueW);
            return (int)value.size() * 23 + (int)note.size() * 19 + 10;
        };
        auto sectionHeight = [&](const DetailsSection& section) {
            int h = 44;
            std::vector<std::string> value, note;
            for (auto& line : section.lines)
                h += lineHeight(line, value, note);
            return h;
        };
        int scroll = (int)glideTowards(in.detailsShown, in.detailsTop);
        c.setClip(Rect{panel.x, panel.y + 4, panel.w, panel.h - 8});
        auto drawSection = [&](const DetailsSection& section, int sx, int sy) {
            std::string title = section.title;
            for (char& ch : title)
                ch = (char)toupper((unsigned char)ch);
            c.text(_font, (float)sx, sy + 30.0f, title, 13.5f, t.accent, true);
            int y = sy + 44;
            for (auto& line : section.lines) {
                std::vector<std::string> value, note;
                int h = lineHeight(line, value, note);
                c.textFit(_font, (float)sx, y + 17.0f, line.label, 16.5f, LabelW - 8, t.dim);
                float ly = y + 17.0f;
                for (auto& part : value) {
                    c.text(_font, (float)(sx + LabelW), ly, part, 17, line.good ? t.accent : line.bad && note.empty() ? t.bad : t.text, line.good);
                    ly += 23;
                }
                for (auto& part : note) {
                    c.text(_font, (float)(sx + LabelW), ly - 4, part, 14, line.bad ? t.bad : t.dim);
                    ly += 19;
                }
                y += h;
            }
        };
        int y = panel.y + 8 - scroll;
        for (size_t row = 0; row < 2; row++) {
            size_t first = row * 2;
            int height = 0;
            for (size_t i = first; i < first + 2 && i < in.sections.size(); i++) {
                drawSection(in.sections[i], panel.x + Pad + (int)(i - first) * (colW + Gap), y);
                height = std::max(height, sectionHeight(in.sections[i]));
            }
            y += height;
            if (row == 0) {
                c.fill(Rect{panel.x + Pad, y + 4, panel.w - Pad * 2, 1}, t.line);
                y += 10;
            }
        }
        if (!in.notes.empty()) {
            c.fill(Rect{panel.x + Pad, y + 4, panel.w - Pad * 2, 1}, t.line);
            y += 10;
            c.text(_font, (float)(panel.x + Pad), y + 30.0f, "提示", 13.5f, t.accent, true);
            y += 44;
            for (auto& note : in.notes) {
                auto lines = wrapText(_font, note, 15.5f, (float)(panel.w - Pad * 2 - 18));
                c.fillCircle(panel.x + Pad + 3.0f, y + 11.0f, 2.5f, t.dim);
                for (auto& line : lines) {
                    c.text(_font, (float)(panel.x + Pad + 18), y + 16.0f, line, 15.5f, t.text);
                    y += 22;
                }
                y += 8;
            }
        }
        c.clearClip();
        // What goes past the bottom is there to go down to, which a bar along the side says.
        int content = y + scroll - (panel.y + 8);
        in.detailsMost = std::max(0, content - (panel.h - 16));
        in.detailsTop = std::min(in.detailsTop, in.detailsMost);
        if (in.detailsMost > 0) {
            int track = panel.h - 32;
            int thumb = std::max(30, track * (panel.h - 16) / std::max(content, 1));
            int at = (int)((track - thumb) * (double)scroll / in.detailsMost);
            c.fillRound(Rect{panel.right() - 10, panel.y + 16 + at, 4, thumb}, 2, t.faint);
        }
    }

    c.fill(Rect{0, FooterY, ScreenW, 1}, t.line);
    bool opened = in.ready && in.problem.empty();
    std::string action = detailsActionLocked();
    std::vector<Hint> hints;
    if (!in.ready) {
        // While it opens, all there is to do is stop it.
        drawHints(c, {{"B", "停止", [this] { closeInside(); }}});
        return;
    }
    hints.push_back({"A", "查看内部", [this] { goInside(); }, opened});
    if (!action.empty()) {
        hints.push_back({"X", action, [this] {
            closeInside();
            onlyTheSelected([this] { pressCompress(); });
        }});
    }
    hints.push_back({"+", "菜单", [this] { openMenu(MenuInside); }});
    hints.push_back({"B", "返回", [this] { closeInside(); }});
    drawHints(c, hints);
}

void App::drawInside(Canvas& c) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    sys::Lock lock(_mutex);
    if (!_inside)
        return;
    Inside& in = *_inside;
    if (in.full && in.ready) {
        drawInsideFull(c);
        return;
    }
    constexpr int Top = 88, Bottom = FooterY - 16;
    const std::vector<int>* shown = in.ready ? &insideShownLocked() : nullptr;
    int count = shown ? (int)shown->size() : 0;
    int focused = count > 0 ? (*shown)[(size_t)std::min(in.focus, count - 1)] : -1;

    // The top: the game, then each folder down to this one, which is the way back. Too long for the room, the folders nearest the top give
    // way first.
    drawGameMark(c, _font, in.iconSmall, in.tile, in.name, Rect{Margin, 18, 36, 36}, 8);
    std::vector<std::string> trail{in.name};
    if (in.ready && !in.nodes.empty()) {
        if (in.foundFrom >= 0) {
            trail.push_back("找到：“" + in.finding + "\xE2\x80\x9D");
        } else {
            std::vector<std::string> folders;
            for (int i = in.folder; i > 0; i = in.nodes[(size_t)i].parent)
                folders.insert(folders.begin(), in.nodes[(size_t)i].name);
            trail.insert(trail.end(), folders.begin(), folders.end());
        }
    }
    std::string total;
    if (in.ready && in.problem.empty()) {
        if (in.foundFrom >= 0)
            total = count == 1 ? std::string("找到 1 项") : "找到 " + withCommas(count) + " 项";
        else if (in.folder == 0)
            total = withCommas(in.files) + " 个文件" + Dot + formatSize(in.bytes);
        else
            total = itemsWord(in.nodes[(size_t)in.folder].items) + Dot + formatSize(in.nodes[(size_t)in.folder].size);
    }
    constexpr float TrailSize = 18;
    float right = ScreenW - Margin - (total.empty() ? 0 : _font.measure(total, 15) + 28);
    auto trailWidth = [&] {
        float w = 0;
        for (size_t i = 0; i < trail.size(); i++)
            w += _font.measure(trail[i], TrailSize, i == trail.size() - 1) + (i > 0 ? 30 : 0);
        return w;
    };
    float x = Margin + 50.0f;
    while (trail.size() > 2 && x + trailWidth() > right) {
        if (trail[1] == "\xE2\x80\xA6")
            trail.erase(trail.begin() + 2);
        else
            trail[1] = "\xE2\x80\xA6";
        if (trail.size() > 2 && trail[1] == "\xE2\x80\xA6" && x + trailWidth() > right && trail.size() == 3)
            break;
    }
    for (size_t i = 0; i < trail.size(); i++) {
        bool last = i == trail.size() - 1;
        if (i > 0) {
            drawChevron(c, x + 12, 37, t.faint);
            x += 30;
        }
        std::string piece = fitText(_font, trail[i], TrailSize, std::max(40.0f, right - x), last);
        x = c.text(_font, x, 43, piece, TrailSize, last ? t.text : t.dim, last);
    }
    if (!total.empty())
        c.textRight(_font, ScreenW - Margin, 42, total, 15, t.dim);
    c.fill(Rect{0, TopBarH, ScreenW, 1}, t.line);

    // On the left, what's in this folder.
    Rect list{Margin, Top, 640, Bottom - Top};
    c.fillRound(list, 16, t.card);
    const int sizeRight = list.right() - 22, kindX = sizeRight - 96 - 118, nameX = list.x + 66;
    if (!in.ready) {
        drawSpinner(c, list.x + list.w / 2.0f, list.y + list.h / 2.0f - 20, 15);
        c.textCenter(_font, list.x + list.w / 2.0f, list.y + list.h / 2.0f + 28, "正在打开", 18, t.dim);
    } else if (!in.problem.empty()) {
        float y = list.y + list.h / 2.0f - 40;
        c.textCenter(_font, list.x + list.w / 2.0f, y, "无法打开", 22, t.text, true);
        std::string why = plainReason(in.problem);
        drawExplanation(c, list.x + 40.0f, y + 36, why.rfind("Something went wrong", 0) == 0 ? in.problem : why, 16.5f, list.w - 80.0f, 24, t.dim, 4);
    } else {
        c.text(_font, list.x + 22.0f, list.y + 29.0f, "名称", 14, t.dim);
        c.text(_font, (float)kindX, list.y + 29.0f, "类型", 14, t.dim);
        c.textRight(_font, (float)sizeRight, list.y + 29.0f, "大小", 14, t.dim);
        const int view = InsideListBottom - InsideListTop;
        double top = in.focus * (double)InsideItemH;
        if (in.focus != in.lastFocus) {
            in.lastFocus = in.focus;
            if (top < in.scroll)
                in.scroll = top;
            if (top + InsideItemH > in.scroll + view)
                in.scroll = top + InsideItemH - view;
        }
        in.scroll = std::max(0.0, std::min(in.scroll, std::max(0.0, (double)count * InsideItemH - view)));
        c.setClip(Rect{list.x, InsideListTop, list.w, view});
        int scrolled = (int)glideTowards(in.scrollShown, in.scroll);
        // A folder gone into slides in from the right, and one gone back up to from the left.
        int dx = 0;
        if (in.slideSince >= 0 && _time - in.slideSince < RiseTime) {
            dx = (int)(in.slideFrom * (1 - easeOut((_time - in.slideSince) / RiseTime)) * 40);
            _dirty = true;
        }
        if (count > 0) {
            Rect selection{list.x + 8 + dx, InsideListTop + (int)glide(6, top) - scrolled, list.w - 16, InsideItemH};
            c.fillRound(selection, 10, t.focus);
            c.strokeRound(selection, 10, 2, t.accent);
        }
        for (int i = 0; i < count; i++) {
            int y = InsideListTop + i * InsideItemH - scrolled;
            if (y + InsideItemH < InsideListTop || y > InsideListBottom)
                continue;
            const InsideNode& node = in.nodes[(size_t)(*shown)[(size_t)i]];
            std::string kind = node.folder ? std::string("Folder") : node.kind;
            drawKindTile(c, kind, list.x + 22 + dx, y + 8, 30, _dark);
            c.textFit(_font, (float)(nameX + dx), y + 30.0f, node.name, 18, (float)(kindX - 16 - nameX), t.text);
            c.textFit(_font, (float)(kindX + dx), y + 29.0f, node.folder ? itemsWord(node.items) : kindPlural(node.kind, 1), 15, 108, t.dim);
            c.textRight(_font, (float)(sizeRight + dx), y + 29.0f, formatSize(node.size), 16, t.dim);
            int topEdge = std::max(y, InsideListTop), bottomEdge = std::min(y + InsideItemH, InsideListBottom);
            if (bottomEdge > topEdge) {
                _hits.push_back(Hit{Rect{list.x, topEdge, list.w, bottomEdge - topEdge}, [this, i] {
                    sys::Lock lock(_mutex);
                    if (!_inside || !_inside->ready)
                        return;
                    _inside->focus = i;
                    const std::vector<int>& now = insideShownLocked();
                    if (i < (int)now.size()) {
                        _insideWant = now[(size_t)i];
                        _insideWake.wakeAll();
                    }
                    _dirty = true;
                }});
            }
        }
        c.clearClip();
        if (count == 0)
            c.textCenter(_font, list.x + list.w / 2.0f, list.y + list.h / 2.0f, "这里没有内容", 18, t.dim);
        else if (count * InsideItemH > view) {
            int track = view - 8;
            int thumb = std::max(30, track * view / (count * InsideItemH));
            int at = (int)((track - thumb) * (double)scrolled / std::max(1.0, (double)count * InsideItemH - view));
            c.fillRound(Rect{list.right() - 7, InsideListTop + 4 + at, 4, thumb}, 2, t.faint);
        }
    }

    // On the right, what's under the pointer, the way its kind is shown.
    Rect preview{list.right() + 20, Top, ScreenW - Margin - (list.right() + 20), Bottom - Top};
    c.fillRound(preview, 16, t.card);
    if (focused >= 0) {
        bool ready = in.previewOf == focused && in.previewReady;
        // Moving on, what was shown stays a moment while the next is read, so going down a folder doesn't flash a spinner at each step.
        bool stale = !ready && in.previewReady && in.previewOf >= 0 && in.previewOf < (int)in.nodes.size() && _time - in.wantSince < 0.35;
        const InsideNode& node = in.nodes[(size_t)(stale ? in.previewOf : focused)];
        c.textFit(_font, preview.x + 20.0f, preview.y + 36.0f, node.name, 19, preview.w - 40.0f, t.text, true);
        Rect area{preview.x + 20, preview.y + 90, preview.w - 40, preview.bottom() - 18 - (preview.y + 90)};
        if (!ready && !stale) {
            drawSpinner(c, area.x + area.w / 2.0f, area.y + area.h / 2.0f - 10, 13);
        } else {
            // What's said about it, in a few words, on a second row when they don't fit on one.
            float chipX = preview.x + 20.0f;
            int chipY = preview.y + 52;
            for (auto& chip : in.chips) {
                float w = std::min(_font.measure(chip, 14) + 20, preview.w - 40.0f);
                if (chipX + w > preview.right() - 20 && chipX > preview.x + 20.0f) {
                    chipX = preview.x + 20.0f;
                    chipY += 32;
                }
                c.fillRound(Rect{(int)chipX, chipY, (int)w, 26}, 8, t.raised);
                c.text(_font, chipX + 10, chipY + 18.0f, fitText(_font, chip, 14, w - 20), 14, t.dim);
                chipX += w + 8;
            }
            area.y = chipY + 38;
            area.h = preview.bottom() - 18 - area.y;
            constexpr float Mono = 15, LineH = 21;
            float cell = monoCell(_mono, Mono);
            if (in.picture && !in.picture->empty() && !node.folder) {
                drawCheckerboard(c, area, t.raised, t.card);
                c.image(*in.picture, area.x + (area.w - in.picture->w) / 2, area.y + (area.h - in.picture->h) / 2, 4);
            } else if (!in.lines.empty() || in.rawShown) {
                c.fillRound(area, 10, t.background);
                c.setClip(Rect{area.x, area.y, area.w - 8, area.h});
                float y = area.y + 26.0f;
                if (in.rawShown) {
                    std::string where, bytes, text;
                    for (int i = 0; i < InsidePreviewLines && (size_t)i * 8 < in.raw.size(); i++) {
                        hexLine(in.raw, (size_t)i * 8, 8, where, bytes, text);
                        float lx = area.x + 14.0f;
                        monoText(c, _mono, lx, y, where, Mono, cell, t.faint, area.right() - 8.0f);
                        monoText(c, _mono, lx + cell * 10, y, bytes, Mono, cell, t.text, area.right() - 8.0f);
                        monoText(c, _mono, lx + cell * 36, y, text, Mono, cell, t.dim, area.right() - 8.0f);
                        y += LineH;
                    }
                } else {
                    int digits = std::max(3, (int)std::to_string(std::max(in.totalLines, 1)).size());
                    for (int i = 0; i < InsidePreviewLines && i < (int)in.lines.size(); i++) {
                        std::string number = std::to_string(i + 1);
                        float numberRight = area.x + 14.0f + cell * digits;
                        monoText(c, _mono, numberRight - cell * number.size(), y, number, Mono, cell, t.faint, numberRight);
                        monoText(c, _mono, numberRight + 14, y, in.lines[(size_t)i], Mono, cell, t.text, area.right() - 12.0f);
                        y += LineH;
                    }
                }
                c.clearClip();
            } else if (node.folder) {
                // A folder: its picture, when it has one straight inside it, and what it's made of, the most first.
                float y = (float)area.y;
                if (in.picture && !in.picture->empty()) {
                    Rect at{area.x + (area.w - in.picture->w) / 2, area.y + 4, in.picture->w, in.picture->h};
                    c.shadow(at, 12, 14, Color{0, 0, 0, 80});
                    c.image(*in.picture, at.x, at.y, 12);
                    y += at.h + 30;
                }
                i64 most = in.breakdown.empty() ? 1 : std::max<i64>(1, in.breakdown[0].bytes);
                for (size_t i = 0; i < in.breakdown.size() && y + 46 <= area.bottom(); i++) {
                    const Inside::KindCount& kind = in.breakdown[i];
                    drawKindTile(c, kind.kind, area.x, (int)y + 2, 28, _dark);
                    c.text(_font, area.x + 42.0f, y + 21, kindPlural(kind.kind, kind.files), 16.5f, t.text);
                    c.text(_font, area.x + 250.0f, y + 21, kind.files == 1 ? std::string("1 个文件") : withCommas(kind.files) + " 个文件", 14.5f, t.dim);
                    c.textRight(_font, (float)area.right(), y + 21, formatSize(kind.bytes), 16, t.text);
                    // Its share of the folder, as a line under it, so the biggest stand out.
                    KindLook look = kindLook(kind.kind, _dark);
                    int barW = (int)((area.w - 42) * (double)kind.bytes / most);
                    c.fillRound(Rect{area.x + 42, (int)y + 30, area.w - 42, 4}, 2, t.raised);
                    c.fillRound(Rect{area.x + 42, (int)y + 30, std::max(4, barW), 4}, 2, look.ink.withAlpha(170));
                    y += 46;
                }
            } else {
                // Nothing in it to show, or not shown: it says why, in the chips, and that's all there is.
                c.textCenter(_font, area.x + area.w / 2.0f, area.y + area.h / 2.0f, node.size == 0 ? "It's empty" : "没有可显示的内容", 17, t.dim);
            }
        }
    }

    // Along the bottom, where what's under the pointer is, and what can be done with it.
    c.fill(Rect{0, FooterY, ScreenW, 1}, t.line);
    constexpr float Line1 = 671, Line2 = 695;
    std::vector<Hint> hints;
    if (!in.ready) {
        c.text(_font, Margin, Line1, "正在打开 " + fitText(_font, in.name, 17, 380, true), 17, t.text, true);
        c.text(_font, Margin, Line2, "B stops it", 14.5f, t.dim);
        hints.push_back({"B", "停止", [this] { closeInside(); }});
    } else if (in.saving) {
        double done = in.savingBytes > 0 ? (double)in.savedBytes / in.savingBytes : 0;
        c.text(_font, Margin, Line1, in.savingFiles <= 1 ? std::string("正在保存到 SD 卡") : format("正在保存 %lld / %lld 个文件", (long long)std::min(in.savedFiles + 1, in.savingFiles), (long long)in.savingFiles), 17, t.text, true);
        drawBar(c, Rect{Margin, (int)Line2 - 10, 300, 5}, done, t.accent);
        c.text(_font, Margin + 312, Line2 - 3, format("%s of %s", formatSize(in.savedBytes).c_str(), formatSize(in.savingBytes).c_str()), 14.5f, t.dim);
        hints.push_back({"B", "停止保存", [this] { _insideSaveStop = true; }});
    } else {
        if (focused >= 0) {
            const InsideNode& node = in.nodes[(size_t)focused];
            c.textFit(_font, Margin, Line1, node.name, 17, 430, t.text, true);
            std::string where;
            for (int i = node.parent; i > 0; i = in.nodes[(size_t)i].parent)
                where = in.nodes[(size_t)i].name + (where.empty() ? "" : " \xE2\x80\xBA " + where);
            c.textFit(_font, Margin, Line2, where.empty() ? in.name : where, 14.5f, 430, t.dim);
        }
        bool canOpen = focused >= 0 && (in.nodes[(size_t)focused].folder ||
                                         (in.previewOf == focused && in.previewReady && (!in.lines.empty() || in.rawShown || in.pictureFull)));
        hints.push_back({"A", "打开", [this] {
            InputState none{};
            updateInside(ButtonA, none, 0);
        }, canOpen});
        hints.push_back({"X", "保存到 SD 卡", [this] {
            sys::Lock lock(_mutex);
            saveInsideFocusedLocked();
        }, focused >= 0});
        hints.push_back({"Y", "查找", [this] { findInside(); }, in.ready && in.problem.empty()});
        hints.push_back({"+", "菜单", [this] { openMenu(MenuInside); }});
        hints.push_back({"B", "返回", [this] {
            InputState none{};
            updateInside(ButtonB, none, 0);
        }});
    }
    drawHints(c, hints);
}

/// What's under the pointer on the whole screen: every line of it, its bytes, or its picture as big as it goes. With the lock held.
void App::drawInsideFull(Canvas& c) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    Inside& in = *_inside;
    const std::vector<int>& shown = insideShownLocked();
    if (shown.empty())
        return;
    const InsideNode& node = in.nodes[(size_t)shown[(size_t)std::min(in.focus, (int)shown.size() - 1)]];
    int total = in.fullLines();
    int last = std::min(in.fullTop + InsideFullLines, total);

    drawGameMark(c, _font, in.iconSmall, in.tile, in.name, Rect{Margin, 18, 36, 36}, 8);
    std::string where;
    for (int i = node.parent; i > 0; i = in.nodes[(size_t)i].parent)
        where = in.nodes[(size_t)i].name + (where.empty() ? "" : " \xE2\x80\xBA " + where);
    float x = Margin + 50.0f;
    if (!where.empty()) {
        x = c.text(_font, x, 43, fitText(_font, where, 18, 560), 18, t.dim);
        drawChevron(c, x + 12, 37, t.faint);
        x += 30;
    }
    c.text(_font, x, 43, fitText(_font, node.name, 18, ScreenW - Margin - 260 - x, true), 18, t.text, true);
    std::string place = in.pictureFull && !in.rawShown && in.lines.empty()
                            ? format("%d × %d", in.pictureFull->w, in.pictureFull->h)
                        : in.rawShown ? format("字节 %s–%s / %s", withCommas((long long)in.fullTop * 16 + 1).c_str(),
                                               withCommas(std::min((long long)last * 16, (long long)in.raw.size())).c_str(), withCommas((long long)in.raw.size()).c_str())
                                      : format("行 %s–%s / %s", withCommas(in.fullTop + 1).c_str(), withCommas(last).c_str(), withCommas(total).c_str());
    c.textRight(_font, ScreenW - Margin, 42, place, 15, t.dim);
    c.fill(Rect{0, TopBarH, ScreenW, 1}, t.line);

    Rect area{Margin, 88, ScreenW - Margin * 2, FooterY - 16 - 88};
    constexpr float Mono = 15, LineH = 21;
    float cell = monoCell(_mono, Mono);
    if (in.pictureFull && !in.rawShown && in.lines.empty()) {
        drawCheckerboard(c, area, t.raised, t.card);
        c.image(*in.pictureFull, area.x + (area.w - in.pictureFull->w) / 2, area.y + (area.h - in.pictureFull->h) / 2, 4);
    } else {
        c.fillRound(area, 12, t.background);
        c.setClip(Rect{area.x, area.y, area.w - 14, area.h});
        float y = area.y + 27.0f;
        if (in.rawShown) {
            // In the middle, as a column of bytes is far narrower than the screen.
            std::string offset, bytes, text;
            float lx = area.x + std::max(18.0f, (area.w - cell * 78) / 2);
            for (int i = in.fullTop; i < last; i++) {
                hexLine(in.raw, (size_t)i * 16, 16, offset, bytes, text);
                monoText(c, _mono, lx, y, offset, Mono, cell, t.faint, area.right() - 14.0f);
                monoText(c, _mono, lx + cell * 11, y, bytes, Mono, cell, t.text, area.right() - 14.0f);
                monoText(c, _mono, lx + cell * 62, y, text, Mono, cell, t.dim, area.right() - 14.0f);
                y += LineH;
            }
        } else {
            int digits = std::max(3, (int)std::to_string(std::max(in.totalLines, 1)).size());
            for (int i = in.fullTop; i < last; i++) {
                std::string number = std::to_string(i + 1);
                float numberRight = area.x + 18.0f + cell * digits;
                monoText(c, _mono, numberRight - cell * number.size(), y, number, Mono, cell, t.faint, numberRight);
                monoText(c, _mono, numberRight + 16, y, in.lines[(size_t)i], Mono, cell, t.text, area.right() - 16.0f);
                y += LineH;
            }
        }
        c.clearClip();
        if (total > InsideFullLines) {
            int track = area.h - 16;
            int thumb = std::max(30, track * InsideFullLines / total);
            int at = (int)((track - thumb) * (double)in.fullTop / std::max(1, total - InsideFullLines));
            c.fillRound(Rect{area.right() - 9, area.y + 8 + at, 4, thumb}, 2, t.faint);
        }
    }

    c.fill(Rect{0, FooterY, ScreenW, 1}, t.line);
    constexpr float Line1 = 671, Line2 = 695;
    c.textFit(_font, Margin, Line1, node.name, 17, 560, t.text, true);
    std::string help = in.lines.empty() && !in.rawShown ? std::string("按屏幕尽可能放大")
                     : in.totalLines > (int)in.lines.size() && !in.rawShown ? format("显示前 %s 行，上下键浏览，L / R 翻页", withCommas((long long)in.lines.size()).c_str())
                     : std::string("上下键浏览，L / R 翻页");
    c.textFit(_font, Margin, Line2, help, 14.5f, 640, t.dim);
    drawHints(c, {{"B", "返回", [this] {
        sys::Lock lock(_mutex);
        if (_inside)
            _inside->full = false;
        _dirty = true;
    }}});
}

}  // namespace hbp
