// What's on screen. Everything is drawn again from what's known whenever something changes, so there's no screen state to get out of step
// with the work going on behind it.
#include "app_internal.h"

#include <cmath>

namespace hbp {

using namespace app;

namespace {

struct Theme {
    Color background, card, raised, focus, text, dim, faint, line, accent, onAccent, warn, bad, scrim;
};

const Theme DarkTheme = {
    {27, 29, 32},    {37, 40, 44},   {50, 54, 59},    {38, 52, 53},  {238, 240, 242}, {163, 170, 178}, {112, 119, 127},
    {52, 56, 61},    {45, 190, 172}, {8, 30, 27},     {232, 170, 66}, {238, 110, 94},  {0, 0, 0, 160},
};

const Theme LightTheme = {
    {240, 241, 243}, {255, 255, 255}, {226, 228, 232}, {222, 242, 239}, {28, 32, 36}, {88, 96, 104}, {136, 143, 150},
    {214, 217, 221}, {0, 134, 120},   {255, 255, 255}, {170, 110, 10},  {198, 56, 42}, {0, 0, 0, 110},
};

// Where each part of a row goes.
constexpr int TickX = 66;
constexpr int IconX = 92;
constexpr int IconSize = 60;
constexpr int TextX = 168;
constexpr int TextW = 500;
constexpr int NowRight = 790;
constexpr int AfterRight = 900;
constexpr int SavedRight = 1020;
constexpr int BarX = 1052;
constexpr int BarRight = 1234;

/// A rough idea of a length of time, which is all anyone can say about time left.
std::string roughTime(double seconds) {
    if (seconds < 60)
        return "不到一分钟";
    int minutes = (int)(seconds / 60 + 0.5);
    if (minutes < 60)
        return format("%d min", minutes);
    return format("%d h %d min", minutes / 60, minutes % 60);
}

/// Where the next character of some UTF-8 starts.
size_t nextCharacter(const std::string& text, size_t position) {
    position++;
    while (position < text.size() && ((u8)text[position] & 0xC0) == 0x80)
        position++;
    return position;
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

void App::draw(Canvas& c) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    c.clearClip();
    if (drawOverMenuBehind(c))
        return;
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
    }
    if (!_toast.empty())
        drawToast(c);
    if (_menu) {
        drawMenuBehind(c);
        _menuBehind.resize((size_t)c.width() * c.height());
        for (int y = 0; y < c.height(); y++)
            memcpy(_menuBehind.data() + (size_t)y * c.width(), c.row(y), (size_t)c.width() * sizeof(u32));
        _menuBehindPage = _menu->page;
        _menuBehindItems = (int)_menu->items.size();
        _menuBehindStatus.clear();
        drawMenu(c);
    } else if (!_menuBehind.empty()) {
        std::vector<u32>().swap(_menuBehind);
    }
    if (_dialog)
        drawDialog(c);
}

/// With the + menu open, everything behind it stays as it was until it closes, but for the line at the bottom saying what's going on.
/// False if there's nothing kept from before to draw it over.
bool App::drawOverMenuBehind(Canvas& c) {
    if (!_menu || _menuBehindPage != _menu->page || _menuBehindItems != (int)_menu->items.size() || _menuBehind.empty())
        return false;
    const Theme& t = _dark ? DarkTheme : LightTheme;
    auto restore = [&] {
        for (int y = 0; y < c.height(); y++)
            memcpy(c.row(y), _menuBehind.data() + (size_t)y * c.width(), (size_t)c.width() * sizeof(u32));
    };
    if (_screen == Screen::Main && _tab != TabSettings) {
        sys::Lock lock(_mutex);
        std::string status = format("%d %d %d %d %d %d ", (int)_jobKind.load(), _analystRunning ? 1 : 0, _analysisIndex, _jobIndex, _jobCount, _scanningTab.load()) + _analysisTitle;
        if (status != _menuBehindStatus) {
            _menuBehindStatus = status;
            Rect strip{0, FooterY, ScreenW, ScreenH - FooterY};
            restore();
            c.setClip(strip);
            c.fill(strip, t.background);
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
        drawDialog(c);
    return true;
}

void App::drawToast(Canvas& c) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    constexpr float Size = 17;
    constexpr int PadX = 22, Height = 42;
    int width = (int)_font.measure(_toast, Size) + PadX * 2;
    Rect pill{(ScreenW - width) / 2, FooterY - Height - 12, width, Height};
    c.shadow(pill, 14, 14, Color{0, 0, 0, 70});
    c.fillRound(pill, Height / 2, t.raised);
    c.textCenter(_font, ScreenW / 2.0f, pill.y + 27.0f, _toast, Size, t.text);
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

// -------------------------------------------------------------------------------------------------------------------
// The list

void App::drawStorage(Canvas& c, int x, int width, const char* label, i64 free, i64 total) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    if (free < 0 || total <= 0)
        return;
    c.text(_font, (float)x, 30, label, 14, t.dim);
    c.textRight(_font, (float)(x + width), 30, formatSize(free) + "可用", 14, t.text);
    double used = 1.0 - (double)free / (double)total;
    drawBar(c, Rect{x, 40, width, 5}, used, used > 0.92 ? t.warn : t.accent);
}

void App::drawTopBar(Canvas& c) {
    const Theme& t = _dark ? DarkTheme : LightTheme;

    constexpr float TabSize = 19;
    constexpr float TabGap = 30;
    const char* names[TabCount] = {"已安装游戏", "游戏文件", "设置"};
    float xs[TabCount], widths[TabCount];
    float x = Margin + 42;
    for (int i = 0; i < TabCount; i++) {
        widths[i] = _font.measure(names[i], TabSize, i == _tab);
        xs[i] = x;
        x += widths[i] + TabGap;
    }
    drawButtonGlyph(c, xs[0] - 28, 37, "L", 0, t.raised, t.dim);
    drawButtonGlyph(c, x - TabGap + 28, 37, "R", 0, t.raised, t.dim);
    for (int i = 0; i < TabCount; i++) {
        c.text(_font, xs[i], 44, names[i], TabSize, _tab == i ? t.text : t.dim, _tab == i);
        _hits.push_back(Hit{Rect{(int)xs[i] - 8, 0, (int)widths[i] + 16, TopBarH}, [this, i] { setTab(i); }});
    }
    int from = std::min((int)_tabShown, TabCount - 1), to = std::min(from + 1, TabCount - 1);
    float part = (float)_tabShown - from;
    c.fillRound(Rect{(int)(xs[from] + (xs[to] - xs[from]) * part), 56, (int)(widths[from] + (widths[to] - widths[from]) * part), 3}, 1, t.accent);

    if (_freeUsb >= 0) {
        drawStorage(c, 770, 140, "SD 卡", _freeSd, _totalSd);
        drawStorage(c, 930, 140, "Console", _freeNand, _totalNand);
        drawStorage(c, 1090, 140, "USB", _freeUsb, _totalUsb);
    } else {
        drawStorage(c, 830, 190, "SD 卡", _freeSd, _totalSd);
        drawStorage(c, 1050, 190, "Console", _freeNand, _totalNand);
    }
}

void App::drawColumns(Canvas& c, const std::vector<Row*>& rows) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    constexpr float Size = 14;
    constexpr int Baseline = TopBarH + 21;

    int ticked = 0, tickable = 0;
    for (auto* row : rows) {
        if (row->blocked.empty()) {
            tickable++;
            ticked += row->ticked ? 1 : 0;
        }
    }
    if (tickable > 0) {
        bool all = ticked == tickable;
        drawTick(c, TickX, TopBarH + 16.0f, 9, all, true);
        if (!all && ticked > 0)
            c.line(TickX - 4.0f, TopBarH + 16.0f, TickX + 4.0f, TopBarH + 16.0f, 2.2f, t.accent);
        _hits.push_back(Hit{Rect{TickX - 22, TopBarH, 44, ColumnsH}, [this, all] { tickAll(!all); }});
    }

    c.fill(Rect{0, ListTop - 1, ScreenW, 1}, t.line);
    if (rows.empty())
        return;
    int hidden = 0;
    for (auto& row : _rows)
        hidden += row.tab == _tab && leftOffListLocked(row) ? 1 : 0;
    std::string heading = (rows.size() == 1 ? std::string("1 个游戏") : format("%d games", (int)rows.size())) + ", " + sortDescription();
    if (hidden > 0)
        heading += format(", %d hidden", hidden);
    c.textFit(_font, TextX, Baseline, heading, Size, TextW, t.dim);
    c.textRight(_font, NowRight, Baseline, "当前", Size, _settings.sort == 1 ? t.accent : t.dim, _settings.sort == 1);
    c.textRight(_font, AfterRight, Baseline, "之后", Size, t.dim);
    c.textRight(_font, SavedRight, Baseline, "节省", Size, _settings.sort == 0 ? t.accent : t.dim, _settings.sort == 0);
}

void App::drawRow(Canvas& c, const Row& row, int y) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    bool blocked = !row.blocked.empty();

    drawTick(c, TickX, y + RowH / 2.0f, 13, row.ticked && !blocked, !blocked);
    drawTile(c, row.icon, row.tile, row.name, IconX, y + (RowH - IconSize) / 2, IconSize, 10);

    bool compressed = isCompressed(row);
    bool toInstall = compressedToInstallLocked(row);
    Color main = blocked ? t.faint : row.ticked || compressed ? t.text : t.dim;
    c.textFit(_font, TextX, y + 33.0f, row.name, 20, TextW, main);

    // With two files of the same game on a list, which file it is matters whatever else there is to say.
    std::string whichFile;
    if (row.tab != TabInstalled && row.extracted.empty()) {
        size_t separator = row.detail.rfind("\xC2\xB7");
        if (separator != std::string::npos)
            whichFile = "  \xC2\xB7" + row.detail.substr(separator + 2);
    }

    std::string second = row.detail;
    Color secondColor = t.dim;
    if (blocked) {
        second = row.blocked;
        secondColor = t.faint;
    } else if (row.state == RowState::Failed) {
        second = row.message;
        secondColor = t.bad;
    } else if (row.state == RowState::Done) {
        second = row.freed > 0 ? format("%s. %s freed", _settings.compress ? "已压缩" : "已重打包", formatSize(row.freed).c_str()) : "已完成" + whichFile;
        secondColor = t.accent;
    } else if (toInstall) {
        second = "已压缩，尚未安装" + whichFile;
        secondColor = t.accent;
    } else if (compressed) {
        second = "Already compressed" + whichFile;
        secondColor = t.accent;
    } else if (!row.belowMinimum.empty()) {
        second = whichFile.empty() ? row.belowMinimum : row.belowMinimum.substr(0, row.belowMinimum.find(':')) + whichFile;
        secondColor = t.warn;
    }
    c.textFit(_font, TextX, y + 56.0f, second, 14.5f, TextW, secondColor);

    float number = y + 44.0f;
    c.textRight(_font, NowRight, number, formatSize(row.now), 16.5f, main);

    if (row.state == RowState::Working) {
        drawSpinner(c, AfterRight - 8.0f, y + RowH / 2.0f, 9);
        c.text(_font, AfterRight + 16.0f, number - 1, "正在查看", 15, t.dim);
    } else if (toInstall) {
        // Nothing to save, as it's compressed already: what's left is putting it on the console.
        c.textRight(_font, BarRight, number, "可安装", 16.5f, row.ticked ? t.accent : t.dim, row.ticked);
    } else if (compressed) {
        drawTick(c, BarRight - 13.0f, y + RowH / 2.0f, 13, true, true);
    } else if (row.hasEstimate) {
        bool wanted = row.belowMinimum.empty();
        // One that would come out larger says by how much, below zero and in red, ticked or not.
        bool grows = row.estimate.savedBytes() < 0;
        Color strong = grows ? t.bad : !row.ticked ? t.dim : wanted ? t.accent : t.warn;
        c.textRight(_font, AfterRight, number, formatSize(row.estimate.outputBytes), 16.5f, main);
        c.textRight(_font, SavedRight, number, formatSize(row.estimate.savedBytes()), 16.5f, strong, true);
        double fraction = row.estimate.savedFraction();
        std::string share = grows && fraction > -0.005 ? format("%.1f%%", fraction * 100) : format("%.0f%%", fraction * 100);
        c.textRight(_font, BarRight, y + 34.0f, share, 16.5f, strong, true);
        drawBar(c, Rect{BarX, y + 44, BarRight - BarX, 6}, fraction, strong);
    } else if (!blocked && row.state != RowState::Failed) {
        c.textRight(_font, AfterRight, number, "\xE2\x80\x93", 16.5f, t.faint);
        c.textRight(_font, SavedRight, number, "\xE2\x80\x93", 16.5f, t.faint);
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
    if (_tab == TabSettings) {
        {
            sys::Lock lock(_mutex);
            drawTopBar(c);
        }
        drawOptions(c);
        return;
    }
    sys::Lock lock(_mutex);
    auto rows = visibleRowsLocked(_tab);
    drawTopBar(c);
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

/// What a list with nothing on it says instead, which depends on why there's nothing.
void App::drawEmptyList(Canvas& c) {
    bool usb = false;
    for (auto& drive : _drives)
        usb = usb || drive.isUsb;
    std::vector<std::string> places;
    if (_settings.fromSd)
        places.push_back("SD 卡");
    if (_settings.fromUsb && usb)
        places.push_back("USB 盘");
    if (_settings.fromPcNetwork || _settings.fromPcCable)
        places.push_back("正在共享游戏的 PC");
    std::string listed;
    for (size_t i = 0; i < places.size(); i++)
        listed += (i == 0 ? "" : i + 1 == places.size() ? "和" : ", ") + places[i];
    std::string where = !_settings.filesFolder.empty() ? "该文件夹" : places.empty() ? "无" : listed;
    JobKind kind = _jobKind;
    bool scanning = !_scanned[_tab] || kind == JobKind::Starting || (kind == JobKind::Scanning && _scanningTab == _tab);
    int done = doneHiddenLocked(_tab);
    if (_tab == TabInstalled && !_console)
        drawEmpty(c, "无法读取主机上的游戏", _platform.consoleProblem(), false);
    else if (scanning && _tab == TabInstalled)
        drawEmpty(c, "正在查看已安装内容", "", true);
    else if (scanning)
        drawEmpty(c, "正在查找游戏文件", "正在浏览" + where + "中的 NSP、NSZ、XCI、XCZ 文件及已解包游戏的文件夹。", true);
    else if (done > 0)
        drawEmpty(c, "此处内容已全部处理", format("%d %s hidden, with nothing left to do with them. The + button brings them back.", done, done == 1 ? "game" : "games"), false);
    else if (_tab == TabInstalled)
        drawEmpty(c, "没有已安装的游戏", _hidden.empty() ? "" : "被隐藏的游戏可用 + 键重新显示。", false);
    else
        drawEmpty(c, "此处没有游戏文件", "上的 NSP、NSZ、XCI、XCZ 文件以及已解包游戏的文件夹" + where + "会显示在此处；可在 + 菜单的「搜索范围」中选择显示哪些。", false);
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
    bool analyzing = _analystRunning && _analyzingTab == _tab && !looking;
    // What's going on for another list, as there's otherwise no telling anything is.
    static const char* const ListNames[] = {"已安装游戏", "游戏文件"};
    bool scanningOther = kind == JobKind::Scanning && _scanningTab != _tab && _scanningTab >= 0 && _scanningTab < TabSettings;
    std::string elsewhere;
    if (scanningOther)
        elsewhere = std::string("同时正在浏览") + ListNames[_scanningTab.load()];
    else if (_analystRunning && _analyzingTab != _tab)
        elsewhere = "同时正在分析" + _analysisTitle;

    int ticked = 0, unknown = 0;
    i64 saved = 0;
    for (auto* row : rows) {
        if (!row->ticked || !row->blocked.empty())
            continue;
        ticked++;
        if (row->hasEstimate)
            saved += row->estimate.savedBytes();
        else
            unknown++;
    }

    if (analyzing) {
        c.textFit(_font, Margin, Line1, "Analyzing " + _analysisTitle, 17, Width, t.text, true);
        drawBar(c, Rect{Margin, (int)Line2 - 10, 300, 5}, _analysisCount > 0 ? (double)std::max(_analysisIndex - 1, 0) / _analysisCount : 0, t.accent);
        c.text(_font, Margin + 312, Line2 - 3, format("%d of %d", _analysisIndex, _analysisCount), 14.5f, t.dim);
    } else if (looking) {
        const char* what = !rows.empty() ? "正在更新列表" : _tab == TabInstalled ? "正在查看已安装内容" : "正在查找游戏文件";
        c.text(_font, Margin, Line1, what, 17, t.text, true);
        std::string second = scanningThis && _jobCount > 0 ? format("%d of %d", _jobIndex, _jobCount) : "";
        // It can't start until the one before it is done.
        if (scanningOther)
            second = std::string("之后") + ListNames[_scanningTab.load()];
        else if (_analystRunning)
            second += (second.empty() ? "" : Dot) + std::string("同时正在分析") + _analysisTitle;
        c.textFit(_font, Margin, Line2, second, 14.5f, Width, t.dim);
    } else if (!rows.empty()) {
        c.text(_font, Margin, Line1, format("%d of %d ticked", ticked, (int)rows.size()), 17, t.text, true);
        std::string second;
        Color color = t.dim;
        bool frees = _tab == TabInstalled && replacesInstalled();
        if (ticked == 0) {
            second = "在游戏上按 A 键以勾选";
        } else if (saved > 0) {
            second = format(frees ? "Frees about %s" : "About %s smaller", formatSize(saved).c_str());
            if (unknown > 0)
                second += "，还有更多待处理";
            color = t.accent;
        } else if (!_settings.compress) {
            second = "按原样重打包，不压缩";
        } else if (unknown > 0) {
            second = "尚未分析";
        } else {
            second = "Nothing to gain";
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
        hints.push_back({"X", verb(), [this] { pressCompress(); }, ticked > 0});
    }
    hints.push_back({"+", "菜单", [this] { openMenu(); }});
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
    constexpr int ListEnd = FooterY - 2;
    constexpr float LargestValue = 720;

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
        float right = ScreenW - Margin - (item.action ? 8.0f : 34.0f);
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

    c.fill(Rect{0, FooterY, ScreenW, 1}, t.line);
    c.textWrapped(_font, Margin, 666, _options[(size_t)_optionFocus].help, 15, 860, 21, t.dim, 2);
    bool acts = _options[(size_t)_optionFocus].action;
    drawHints(c, {{acts ? "A" : "<>", acts ? "选择" : "更改", [this] { _options[(size_t)_optionFocus].change(1); }}, {"B", "退出", [this] { pressQuit(); }}});
}

void App::drawChangeArrows(Canvas& c, float left, float right, float mid, Color ink) {
    c.line(left + 4, mid - 7, left - 3, mid, 2.2f, ink);
    c.line(left - 3, mid, left + 4, mid + 7, 2.2f, ink);
    c.line(right - 4, mid - 7, right + 3, mid, 2.2f, ink);
    c.line(right + 3, mid, right - 4, mid + 7, 2.2f, ink);
}

void App::drawWelcome(Canvas& c) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    constexpr int Left = 140, Width = ScreenW - Left * 2;
    constexpr int ItemH = 46;
    constexpr int SectionH = 30;

    c.text(_font, Left, 70, "欢迎使用 hacBrewPackLZ", 31, t.text, true);
    float y = c.textWrapped(_font, Left, 110, "It stores games compressed, in the form the firmware itself reads, so they take up less room and play just the same. Tick games with A and press X. The + button has everything else.", 17, Width, 25, t.dim);
    y = c.textWrapped(_font, Left, y + 4, "以此方式存储的游戏需要 sigpatches 才能启动，且官方更新无法覆盖安装。", 17, Width, 25, t.warn);

    // Where each one goes, with a heading wherever a section starts.
    int top = (int)y + 4;
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
            c.textCenter(_font, ScreenW / 2.0f, button.y + 27.0f, item.label, 19, focused ? t.onAccent : t.text, focused);
            _hits.push_back(Hit{button, [this] { finishWelcome(); }});
            continue;
        }
        float baseline = area.y + 27.0f;
        c.text(_font, Left + 4, baseline, item.label, 18.5f, t.text);
        std::string value = item.value();
        constexpr float LargestValue = 560;
        float right = Left + Width - (item.action ? 4.0f : 30.0f);
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

    c.fill(Rect{0, FooterY, ScreenW, 1}, t.line);
    c.textWrapped(_font, Margin, 666, _welcome[(size_t)_welcomeFocus].help, 15, 800, 21, t.dim, 2);
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
    hints.push_back({"B", browser.path.empty() ? "取消" : "返回上级", [this] {
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
    c.text(_font, Margin, 46, kind == JobKind::Extracting ? "正在解包" : kind == JobKind::Checking ? "正在校验" : kind == JobKind::Removing ? "正在移除" : verbing(), 26, t.text, true);
    if (_jobCount > 1)
        c.textRight(_font, ScreenW - Margin, 46, format("Game %d of %d", std::max(_jobIndex, 1), _jobCount), 17, t.dim);
    c.fill(Rect{0, TopBarH, ScreenW, 1}, t.line);

    constexpr int IconAt = 104, IconBig = 128;
    constexpr int Left = Margin + IconBig + 32;
    constexpr int Width = ScreenW - Margin - Left;
    drawTile(c, _jobIcon, _jobTile, _jobTitle, Margin, IconAt, IconBig, 18);
    c.textFit(_font, Left, IconAt + 30.0f, _jobTitle, 27, Width, t.text, true);
    c.textFit(_font, Left, IconAt + 62.0f, _jobLabel, 18, Width, t.dim);

    bool hasNumbers = _jobTotal > 1024;
    double stepFraction = _jobTotal > 0 ? (double)_jobDone / (double)_jobTotal : 0;
    drawBar(c, Rect{Left, IconAt + 78, Width, 8}, stepFraction, t.accent);
    std::string numbers;
    if (hasNumbers) {
        numbers = format("%s of %s", formatSize(_jobDone).c_str(), formatSize(_jobTotal).c_str());
        if (_jobRate > 0)
            numbers += format("   \xC2\xB7   %.1f MB/s", _jobRate / (1 << 20));
    }
    c.text(_font, Left, IconAt + 112.0f, numbers, 15.5f, t.dim);
    if (_jobTotal > 0)
        c.textRight(_font, ScreenW - Margin, IconAt + 112.0f, format("%.0f%%", stepFraction * 100), 15.5f, t.dim);

    // The steps a game goes through.
    int steps = (int)_jobSteps.size();
    if (steps > 0) {
        constexpr int StepsY = 306;
        constexpr float StepRadius = 12;
        float x0 = Margin + 70.0f, x1 = ScreenW - Margin - 70.0f;
        float gap = steps > 1 ? (x1 - x0) / (steps - 1) : 0;
        for (int i = 0; i + 1 < steps; i++)
            c.line(x0 + i * gap + StepRadius + 6, StepsY, x0 + (i + 1) * gap - StepRadius - 6, StepsY, 2, i < _jobStep ? t.accent : t.line);
        for (int i = 0; i < steps; i++) {
            float x = x0 + i * gap;
            if (i < _jobStep) {
                drawTick(c, x, StepsY, StepRadius, true, true);
            } else if (i == _jobStep) {
                c.fillCircle(x, StepsY, StepRadius, t.accent.withAlpha(70));
                c.fillCircle(x, StepsY, StepRadius * 0.5f, t.accent);
            } else {
                drawTick(c, x, StepsY, StepRadius, false, true);
            }
            c.textCenter(_font, x, StepsY + 36.0f, _jobSteps[(size_t)i], 15, i <= _jobStep ? t.text : t.faint);
        }
    }

    // All of it.
    double secondsLeft = 0;
    double overall = overallLocked(&secondsLeft);
    double elapsed = sys::now() - _jobStarted;
    constexpr int OverallY = 400;
    c.fill(Rect{Margin, OverallY, ScreenW - Margin * 2, 1}, t.line);
    // With one game, there's nothing for this to say that the bar above doesn't.
    if (_jobCount > 1) {
        c.text(_font, Margin, OverallY + 44.0f, "合计", 17, t.text, true);
        c.textRight(_font, ScreenW - Margin, OverallY + 44.0f, format("%.0f%%", overall * 100), 17, t.text, true);
        drawBar(c, Rect{Margin, OverallY + 58, ScreenW - Margin * 2, 8}, overall, t.accent);
    }

    struct Stat {
        const char* label;
        std::string value;
    };
    std::vector<Stat> stats;
    stats.push_back({"剩余时间", overall > 0.02 && elapsed > 20 ? roughTime(secondsLeft) : "正在估算"});
    stats.push_back({"已完成", roughTime(elapsed)});
    if (_jobFilesDone > 0)
        stats.push_back({"文件", _jobFilesTotal > 0 ? format("%lld of %lld", (long long)_jobFilesDone, (long long)_jobFilesTotal) : format("%lld so far", (long long)_jobFilesDone)});
    if (kind == JobKind::Compressing && _jobFreed > 0)
        stats.push_back({"已释放", formatSize(_jobFreed)});
    int statX = Margin;
    for (auto& stat : stats) {
        c.text(_font, (float)statX, OverallY + 104.0f, stat.label, 14.5f, t.dim);
        c.text(_font, (float)statX, OverallY + 136.0f, stat.value, 23, t.text, true);
        statX += 300;
    }

    c.fill(Rect{0, FooterY, ScreenW, 1}, t.line);
    bool replacing = _jobSteps.size() > 2;
    c.textWrapped(_font, Margin, 672, replacing ? (_settings.checkResult ? "The original stays until the new copy has been checked. If the power goes, it's put right next time this opens." : "The original stays until the new copy has been written. If the power goes, it's put right next time this opens.") : "原文件不会被修改。", 15, 880, 21, t.dim, 2);

    std::vector<Hint> hints;
    if (kind == JobKind::Removing)
        hints.clear();
    else if (stopAsked())
        hints.push_back({"B", "正在停止", [] {}, false});
    else if (_stopAfterGame)
        hints.push_back({"B", "处理完此游戏后停止", [this] { askToStop(); }});
    else
        hints.push_back({"B", "停止", [this] { askToStop(); }});
    drawHints(c, hints);
}

void App::drawResults(Canvas& c) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    sys::Lock lock(_mutex);
    c.text(_font, Margin, 40, _resultsTitle, 26, t.text, true);
    c.textFit(_font, Margin, 66, _resultsDetail, 16, ScreenW - Margin * 2, t.dim);
    c.fill(Rect{0, TopBarH + 8, ScreenW, 1}, t.line);

    constexpr int ListY = TopBarH + 12;
    constexpr int ResultH = 78;
    constexpr int Shown = 7;
    int count = (int)_results.size();
    bool anyFailed = false;
    for (auto& result : _results)
        anyFailed = anyFailed || result.state == RowState::Failed;

    if (count == 0)
        drawEmpty(c, "Nothing was changed", "", false);
    for (int i = _resultTop; i < count && i < _resultTop + Shown; i++) {
        const ResultLine& result = _results[(size_t)i];
        int y = ListY + (i - _resultTop) * ResultH;
        drawTile(c, result.icon, result.tile, result.name, Margin, y + 9, IconSize, 10);
        int textX = Margin + IconSize + 16;
        constexpr int RightW = 220;
        int textW = ScreenW - Margin - RightW - textX;
        c.textFit(_font, (float)textX, y + (result.message.empty() ? 33.0f : 28.0f), result.name, 19, (float)textW, t.text);
        if (!result.message.empty()) {
            Color color = result.state == RowState::Failed ? t.bad : t.warn;
            c.textWrapped(_font, (float)textX, y + 49.0f, result.message, 14, (float)textW, 18, color, 2);
        } else {
            std::string change = format("Was %s", formatSize(result.before).c_str());
            if (result.after > 0 && result.after < result.before)
                change += format(", %.0f%% smaller", (1.0 - (double)result.after / (double)result.before) * 100);
            else if (result.after > result.before && result.before > 0)
                change += format(", %.0f%% larger", ((double)result.after / (double)result.before - 1.0) * 100);
            c.textFit(_font, (float)textX, y + 55.0f, change, 14.5f, (float)textW, t.dim);
        }
        Color outcomeColor = result.state == RowState::Done ? t.accent : result.state == RowState::Failed ? t.bad : t.dim;
        c.textRight(_font, ScreenW - Margin, y + 44.0f, result.outcome, 17, outcomeColor, true);
    }
    if (count > Shown) {
        int span = Shown * ResultH;
        int thumb = std::max(40, span * Shown / count);
        int thumbY = ListY + (span - thumb) * _resultTop / std::max(count - Shown, 1);
        c.fillRound(Rect{ScreenW - 14, thumbY, 4, thumb}, 2, t.faint);
    }

    c.fill(Rect{0, FooterY, ScreenW, 1}, t.line);
    if (anyFailed)
        c.textFit(_font, Margin, 685, _log ? "错误详情也记录在" + _logPath : std::string("如需保存错误日志，请在「更多设置」中开启"), 15, 900, t.dim);
    drawHints(c, {{"A", "已完成", [this] { _screen = Screen::Main; }}});
}

void App::drawNoKeys(Canvas& c) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    c.text(_font, Margin, 46, "hacBrewPackLZ", 23, t.text, true);
    c.fill(Rect{0, TopBarH, ScreenW, 1}, t.line);

    constexpr float Left = 240;
    constexpr float Width = 800;
    c.text(_font, Left, 190, "此操作需要主机密钥", 29, t.text, true);
    float y = c.textWrapped(_font, Left, 236, "They're what games are encrypted with. They come from your own console and can't be included here. Getting them takes a minute:", 18, Width, 27, t.dim);

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
    c.textFit(_font, Left, y, "已查找" + (where.empty() ? std::string() : where[0]), 15, Width, t.faint);
    if (!_keysProblem.empty())
        c.textWrapped(_font, Left, y + 26, _keysProblem, 15, Width, 21, t.warn, 3);

    c.fill(Rect{0, FooterY, ScreenW, 1}, t.line);
    drawHints(c, {{"A", "重试", [this] { lookForKeysAgain(); }}, {"+", "退出", [this] { pressQuit(); }}});
}

void App::drawDialog(Canvas& c) {
    const Theme& t = _dark ? DarkTheme : LightTheme;
    // Only what's on top can be touched.
    _hits.clear();
    c.fill(Rect{0, 0, ScreenW, ScreenH}, t.scrim);

    Dialog& dialog = *_dialog;
    constexpr int Width = 680, Pad = 36;
    constexpr float TextSize = 17, LineH = 25, ParagraphGap = 11;
    constexpr int ButtonH = 50;
    int inner = Width - Pad * 2;

    std::vector<std::vector<std::string>> paragraphs;
    int height = Pad + 34 + 12;
    for (auto& paragraph : dialog.paragraphs) {
        paragraphs.push_back(wrapText(_font, paragraph, TextSize, (float)inner));
        height += (int)(paragraphs.back().size() * LineH + ParagraphGap);
    }
    height += 16 + ButtonH + Pad - 8;

    Rect card{(ScreenW - Width) / 2, std::max((ScreenH - height) / 2, 8), Width, height};
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
    Rect card{(ScreenW - Width) / 2, (ScreenH - height) / 2, Width, height};
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

}  // namespace hbp
