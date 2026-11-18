// What people can do: ticking games, the + menu and the questions it asks, choosing a folder, and the settings.
#include "app_internal.h"

#include <cmath>

#include "../core/remote.h"

namespace hbp {

using namespace app;

void App::showMessage(const std::string& title, const std::string& text) {
    auto dialog = std::make_unique<Dialog>();
    dialog->title = title;
    dialog->paragraphs = {text};
    dialog->choices = {{"确定", nullptr}};
    dialog->cancelChoice = 0;
    _dialog = std::move(dialog);
    _dirty = true;
}

void App::showToast(const std::string& text) {
    constexpr double ToastSeconds = 2.2;
    _toast = text;
    _toastUntil = _time + ToastSeconds;
    _dirty = true;
}

void App::showAbout() {
    auto dialog = std::make_unique<Dialog>();
    dialog->title = format("关于 hacBrewPackLZ %s", AppVersion);
    dialog->paragraphs.push_back("以 Switch 可直接读取的压缩格式存放游戏，占用空间更小，游玩效果相同。");
    dialog->paragraphs.push_back("密钥： " + (_keysPath.empty() ? std::string("未找到") : _keysPath));
    if (_log)
        dialog->paragraphs.push_back("运行记录写入 " + _logPath);
    dialog->paragraphs.push_back("自由软件，遵循 GNU GPL v2；USB 盘读取使用 libusbhsfs、NTFS-3G 与 lwext4（同协议）。游戏内的字节与文本以 Inconsolata 显示（SIL OFL 1.1）。");
    dialog->paragraphs.push_back("简体中文汉化版 · 汉化作者：illyasever");
    dialog->choices = {{"确定", nullptr}};
    dialog->cancelChoice = 0;
    _dialog = std::move(dialog);
    _dirty = true;
}

// -------------------------------------------------------------------------------------------------------------------
// Ticking

void App::toggleRow(int index) {
    sys::Lock lock(_mutex);
    auto rows = visibleRowsLocked(_tab);
    if (index < 0 || index >= (int)rows.size())
        return;
    Row& row = *rows[(size_t)index];
    if (!row.blocked.empty())
        return;
    row.ticked = !row.ticked;
    row.byHand = true;
    row.tickedAt = _time;
    _dirty = true;
}

void App::tickAll(bool ticked) {
    sys::Lock lock(_mutex);
    for (auto* row : visibleRowsLocked(_tab)) {
        if (row->blocked.empty()) {
            row->ticked = ticked;
            row->byHand = true;
        }
    }
    _dirty = true;
}

void App::tickWorthIt() {
    sys::Lock lock(_mutex);
    for (auto* row : visibleRowsLocked(_tab))
        row->byHand = false;
    judgeAllLocked();
}

bool App::tickActionChangesLocked(TickAction action) {
    for (auto* row : visibleRowsLocked(_tab)) {
        if (!row->blocked.empty())
            continue;
        bool wanted = action == TickAction::All || (action == TickAction::WorthIt && row->belowMinimum.empty() && !row->notSure && row->state != RowState::Done);
        if (row->ticked != wanted)
            return true;
    }
    return false;
}

/// How many ways the button goes round. With compression off, no game saves anything to go by, so there's nothing between all and none.
int App::tickActionCount() const { return _settings.compress ? 3 : 2; }

/// What pressing the button would do next, leaving out whatever wouldn't change anything.
App::TickAction App::nextTickActionLocked() {
    static const TickAction order[] = {TickAction::All, TickAction::None, TickAction::WorthIt};
    int count = tickActionCount();
    for (int i = 0; i < count; i++) {
        TickAction action = order[(_tickStep[_tab] + i) % count];
        if (tickActionChangesLocked(action))
            return action;
    }
    return order[_tickStep[_tab] % count];
}

const char* App::tickActionLabel(TickAction action) {
    switch (action) {
        case TickAction::All:
            return "全部勾选";
        case TickAction::None:
            return "取消全部勾选";
        case TickAction::WorthIt:
            break;
    }
    return "仅勾选值得处理的";
}

void App::cycleTicks() {
    TickAction action;
    {
        sys::Lock lock(_mutex);
        action = nextTickActionLocked();
        _tickStep[_tab] = ((int)action + 1) % tickActionCount();
    }
    switch (action) {
        case TickAction::All:
            tickAll(true);
            showToast("全部勾选");
            break;
        case TickAction::None:
            tickAll(false);
            showToast("全部不勾选");
            break;
        case TickAction::WorthIt:
            tickWorthIt();
            showToast("仅勾选值得处理的");
            break;
    }
}

/// What's done to ticked games, done to the one selected, leaving the rest ticked as they were afterwards.
void App::onlyTheSelected(std::function<void()> action) {
    {
        sys::Lock lock(_mutex);
        auto rows = visibleRowsLocked(_tab);
        int index = _focus[_tab];
        if (index < 0 || index >= (int)rows.size())
            return;
        _savedTicks.clear();
        for (int i = 0; i < (int)rows.size(); i++) {
            Row& row = *rows[(size_t)i];
            _savedTicks.emplace_back(row.key, std::make_pair(row.ticked, row.byHand));
            row.ticked = i == index;
            row.byHand = true;
        }
        _dirty = true;
    }
    action();
}

/// Only the games given are ticked, for as long as it takes to act on them, as the overview does with what it counts.
void App::onlyThese(const std::vector<std::string>& keys, std::function<void()> action) {
    {
        sys::Lock lock(_mutex);
        _savedTicks.clear();
        for (auto* row : visibleRowsLocked(_tab)) {
            _savedTicks.emplace_back(row->key, std::make_pair(row->ticked, row->byHand));
            row->ticked = std::find(keys.begin(), keys.end(), row->key) != keys.end();
            row->byHand = true;
        }
        _dirty = true;
    }
    action();
}

void App::restoreTicks() {
    sys::Lock lock(_mutex);
    for (auto& saved : _savedTicks) {
        Row* row = findLocked(saved.first);
        if (row && row->state != RowState::Done) {
            row->ticked = saved.second.first;
            row->byHand = saved.second.second;
        }
    }
    _savedTicks.clear();
    judgeAllLocked();
}

std::vector<std::string> App::tickedGames() {
    std::vector<std::string> keys;
    sys::Lock lock(_mutex);
    for (auto* row : visibleRowsLocked(_tab)) {
        if (row->ready && row->ticked && row->blocked.empty() && row->extracted.empty())
            keys.push_back(row->key);
    }
    return keys;
}

// -------------------------------------------------------------------------------------------------------------------
// The lists

void App::setTab(int tab) {
    if (tab < 0 || tab >= TabCount)
        return;
    if (_overview) {
        _overview = false;
        _dirty = true;
    }
    if (tab == _tab)
        return;
    _tab = tab;
    _focusShown = _focus[tab];
    _scrollShown = _scroll[tab];
    _dirty = true;
    // A list that hasn't come up yet goes first, as it's what's being looked at. What it took the place of picks up again by itself afterwards.
    if (!_scanned[tab] && _jobKind == JobKind::Scanning && _scanningTab != tab)
        _scanStop = true;
}

void App::goToPosition(int position) {
    if (position == 0) {
        _overview = true;
        _dirty = true;
    } else {
        setTab(position - 1);
    }
}

/// A list shown from the overview, on the games the line chosen there counts.
void App::showList(int tab, View view) {
    {
        sys::Lock lock(_mutex);
        if (_view[tab] != view) {
            _view[tab] = view;
            _focus[tab] = 0;
            _scroll[tab] = 0;
        }
    }
    setTab(tab);
    _focusShown = _focus[tab];
    _scrollShown = _scroll[tab];
}

/// To do, then Done, then Everything, the way they're laid out along the top of the list. At either end there's nowhere further to go.
/// With compression off nothing is done but what's been done since the app opened, which To do still shows, so there's no Done to go to.
void App::stepView(int direction) {
    auto views = viewsOffered();
    auto at = std::find(views.begin(), views.end(), _view[_tab]);
    if (at == views.end()) {
        setView(views.front());
        return;
    }
    int index = (int)(at - views.begin()) + direction;
    if (index >= 0 && index < (int)views.size())
        setView(views[(size_t)index]);
}

std::vector<App::View> App::viewsOffered() const { return {View::ToDo, View::Done, View::Everything}; }

void App::setView(View view) {
    sys::Lock lock(_mutex);
    int tab = _tab;
    if (_view[tab] == view)
        return;
    auto before = visibleRowsLocked(tab);
    // A view without the game, only gone through, with the pointer still at its top where the change put it, keeps the game to go back to:
    // right twice from To do to Everything goes through Done.
    bool goneThrough = _anchorKept[tab] && _focus[tab] == 0;
    if (!before.empty() && !goneThrough) {
        int focus = std::max(0, std::min(_focus[tab], (int)before.size() - 1));
        _viewAnchor[tab] = before[(size_t)focus]->key;
        _anchorOnScreen[tab] = focus * (double)RowH - _scroll[tab];
    }
    const std::string& key = _viewAnchor[tab];
    double onScreen = _anchorOnScreen[tab];
    _view[tab] = view;
    _settings.view[tab] = (int)view;
    _viewsChanged = true;

    // The same game if this view has it too, where it was on the screen, with the rest of the view around it. Otherwise the view starts at
    // its top: where a game that isn't in it was says nothing about where anything in it is, and the end of it, which the game after it
    // would often have been, is the last place anyone looks.
    int count = 0, found = -1;
    for (auto& row : _rows) {
        if (row.tab != tab || !inViewLocked(row, view))
            continue;
        if (!key.empty() && row.key == key)
            found = count;
        count++;
    }
    double most = std::max(0.0, (double)count * RowH - ListH);
    _focus[tab] = std::max(found, 0);
    _anchorKept[tab] = found < 0 && !key.empty();
    _focusSeen[tab] = _focus[tab];
    _scroll[tab] = found < 0 ? 0.0 : std::max(0.0, std::min(most, found * (double)RowH - onScreen));
    _focusShown = _focus[tab];
    _scrollShown = _scroll[tab];
    _dirty = true;
}

// -------------------------------------------------------------------------------------------------------------------
// The overview

/// What the ticked installed games on one place save, which is space freed there once each is replaced. Saving NSPs of them frees nothing.
i64 App::freedOnLocked(Storage storage) const {
    if (!replacesInstalled())
        return 0;
    i64 saved = 0;
    for (auto& row : _rows) {
        if (row.tab == TabInstalled && row.storage == storage && row.ticked && row.blocked.empty() && row.hasEstimate && inViewLocked(row, View::ToDo))
            saved += row.sureSaved();
    }
    return std::max<i64>(saved, 0);
}

/// Where game files are looked for, as said on the overview and on an empty list.
std::string App::whereGameFilesAre() const {
    if (!_settings.filesFolder.empty())
        return _settings.filesFolder;
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
    if (_settings.fromGameCard && _console)
        places.push_back("游戏卡带");
    if (places.empty())
        return "未选择位置";
    std::string listed;
    for (size_t i = 0; i < places.size(); i++)
        listed += (i == 0 ? "" : i + 1 == places.size() ? " 和 " : "、") + places[i];
    return listed;
}

/// Whatever the overview goes by, boiled down to a number that changes when any of it does. Working it out goes through every game but
/// only looks, so it's cheap enough to do each time the screen is drawn, unlike the overview itself.
u64 App::overviewKeyLocked() const {
    u64 key = 1469598103934665603ull;
    auto mix = [&](u64 value) {
        key ^= value;
        key *= 1099511628211ull;
    };
    mix(_rows.size());
    for (auto& row : _rows) {
        mix((u64)row.tab | (u64)row.state << 4 | (u64)row.ticked << 8 | (u64)row.hasEstimate << 9 | (u64)row.ready << 10 | (u64)row.blocked.empty() << 11 |
            (u64)row.tiny << 12 | (u64)row.storage << 16 | (u64)(row.hasEstimate && row.estimate.alreadyCompressed) << 24 | (u64)row.markedIncomplete << 25);
        mix((u64)row.now);
        mix(row.hasEstimate ? (u64)row.estimate.outputBytes ^ (u64)row.estimate.inputBytes << 1 : 0);
        mix(row.application ^ row.titleId << 1 ^ (u64)row.gameVersion << 7);
        mix((u64)(uintptr_t)row.icon.get());
        mix(std::hash<std::string>{}(row.name));
    }
    mix((u64)_settings.compress | (u64)_settings.hideSmall << 1 | (u64)_settings.installFiles << 2 | (u64)_settings.replaceInstalled << 3 |
        (u64)_settings.installToConsole << 4 | (u64)_settings.fromSd << 5 | (u64)_settings.fromUsb << 6 | (u64)_settings.fromPcNetwork << 7 |
        (u64)_settings.fromPcCable << 8 | (u64)(_console != nullptr) << 9 | (u64)_scanned[0] << 10 | (u64)_scanned[1] << 11 | (u64)_analystRunning << 12 |
        (u64)_platform.firmwareReadsCompression() << 13);
    mix(std::hash<std::string>{}(_settings.filesFolder));
    mix((u64)_jobKind.load() | (u64)_scanningTab.load() << 8 | (u64)_drives.size() << 16);
    for (i64 value : {_freeSd, _totalSd, _freeNand, _totalNand, _freeUsb, _totalUsb})
        mix((u64)value);
    return key;
}

const App::Overview& App::overviewLocked() {
    u64 key = overviewKeyLocked();
    if (!_overviewCached || key != _overviewCacheKey) {
        _overviewCache = workOutOverviewLocked();
        _overviewCacheKey = key;
        _overviewCached = true;
    }
    return _overviewCache;
}

/// What reading a game's data in full takes, in bytes a second: what this console has done since the app opened, starting from what one was
/// seen to do, as if it had done that for half a minute. Reading games installed on the SD card, a console measured about 54 MB/s, and with
/// every block compressed it's about half that.
double App::measureRateLocked(bool exact) const {
    constexpr double Seen[2] = {54.0 * (1 << 20), 27.0 * (1 << 20)};
    constexpr double PriorSeconds = 30;
    int i = exact ? 1 : 0;
    return (Seen[i] * PriorSeconds + _measuredBytes[i]) / (PriorSeconds + _measuredSeconds[i]);
}

/// About how long reading what's left takes, on one list: each game's data at the speed this console reads it, less what's been read of the
/// one being measured, so the time goes down as it reads, however big the game.
std::string App::measureTimeLocked(int tab) {
    double seconds = 0;
    for (auto& row : _rows) {
        if (row.tab != tab || !unmeasuredLocked(row))
            continue;
        double bytes = (double)row.measureBytes();
        if (row.state == RowState::Working && _analystRunning && _analystMeasuring)
            bytes -= (double)measuringDoneLocked();
        seconds += std::max(bytes, 0.0) / measureRateLocked(row.measuredExactly());
    }
    int minutes = (int)ceil(seconds / 60);
    return minutes <= 1 ? "约 1 分钟" : minutes < 60 ? format("约 %d 分钟", minutes) : format("约 %d 小时 %d 分", minutes / 60, minutes % 60);
}

/// What there is to do on each list, counted as the lists count it: what's ticked to do, what that comes to and the games saving the most,
/// what's done, and, when game files are saved as NSPs, the compressed ones whose game isn't installed that way, which can still be installed.
App::Overview App::workOutOverviewLocked() {
    Overview o;
    JobKind kind = _jobKind;
    i64 frees = 0, smaller = 0;
    bool anyAtLeast = false;
    int tickedAll = 0, toDoAll = 0, doneAll = 0, wontStartAll = 0;
    bool looking = false;
    for (int tab = 0; tab < TabCount; tab++) {
        OverviewCard& card = o.cards[tab];
        auto add = [&](OverviewLine::Kind lineKind, const std::string& text, const std::string& sub = "") -> OverviewLine& {
            OverviewLine line;
            line.tab = tab;
            line.kind = lineKind;
            line.text = text;
            line.sub = sub;
            card.lines.push_back(line);
            return card.lines.back();
        };
        if (tab == TabInstalled && !_console) {
            add(OverviewLine::Nothing, "无法读取主机上的游戏", _platform.consoleProblem());
            continue;
        }
        bool scanning = !_scanned[tab] || kind == JobKind::Starting || (kind == JobKind::Scanning && _scanningTab == tab);
        looking = looking || scanning;
        bool savesNsps = tab == TabFiles && !installsGameFiles();
        bool canInstall = savesNsps && _console && _settings.compress && _platform.firmwareReadsCompression();
        int all = 0, toDo = 0, ticked = 0, done = 0, unknown = 0, notSure = 0, wontStart = 0, developer = 0;
        std::vector<std::string> developerKeys;
        i64 saved = 0, now = 0, known = 0, freed = 0;
        bool atLeast = false;
        std::vector<const Row*> savers;
        std::vector<std::string> toInstall;
        int unmeasured = 0;
        for (auto& row : _rows) {
            if (row.tab != tab)
                continue;
            all++;
            if (row.markedIncomplete && row.blocked.empty() && row.state != RowState::Working)
                wontStart++;
            if (inViewLocked(row, View::Everything)) {
                card.games++;
                card.bytes += row.now;
            }
            // What analysis hasn't read in full yet, which it goes on to.
            if (unmeasuredLocked(row))
                unmeasured++;
            if (inViewLocked(row, View::Done)) {
                done++;
                // What compressing has freed so far, which each copy says.
                freed += row.state == RowState::Done ? row.freed : row.hasEstimate ? row.estimate.freedBytes : 0;
                // A copy an older version made waits under Done until it's known to be worth compressing again, and counts for Measure.
                if (row.hasEstimate && row.notSure && row.estimate.olderCopy && row.blocked.empty())
                    notSure++;
                // DLC on its own isn't a game that can be found installed.
                bool dlcAlone = row.extracted.empty() && row.titleId != row.application;
                if (canInstall && row.state == RowState::Ready && row.blocked.empty() && isCompressed(row) && !dlcAlone && !installedCompressedLocked(row))
                    toInstall.push_back(row.key);
            }
            if (!inViewLocked(row, View::ToDo))
                continue;
            toDo++;
            // What a first look can't vouch for has a line of its own, which measures it.
            if (row.hasEstimate && row.notSure && row.blocked.empty())
                notSure++;
            // So does what its developer compressed that's worth compressing again, as it's left unticked: its updates then need its original.
            if (!row.ticked && row.blocked.empty() && row.hasEstimate && row.estimate.byMakers && !row.estimate.alreadyCompressed && !row.notSure && _settings.compress) {
                developer++;
                developerKeys.push_back(row.key);
            }
            if (!row.ticked || !row.blocked.empty())
                continue;
            ticked++;
            // A packed file being installed counts as what it takes installed as it is, as its figures do.
            now += row.hasEstimate && row.estimate.packedBytes > 0 ? row.estimate.inputBytes : row.now;
            if (!row.hasEstimate) {
                unknown++;
                continue;
            }
            known += row.estimate.packedBytes > 0 ? row.estimate.inputBytes : row.now;
            saved += row.sureSaved();
            atLeast = atLeast || row.estimate.atLeast();
            if (row.sureSaved() > 0)
                savers.push_back(&row);
        }
        if (scanning && all == 0) {
            add(OverviewLine::Looking, tab == TabInstalled ? "正在查看已安装游戏" : "正在查找游戏文件",
                tab == TabInstalled ? "" : "正在浏览 " + whereGameFilesAre() + " 中的 NSP、NSZ、XCI、XCZ 文件。");
            continue;
        }
        bool freesSpace = tab == TabInstalled && replacesInstalled();
        if (ticked > 0) {
            // The word is the one X goes by, and what follows says where the games end up, which is easily forgotten.
            const char* word = tab == TabFiles && installsGameFiles() ? "安装" : _settings.compress ? "压缩" : "重打包";
            OverviewLine& line = add(OverviewLine::ToDo, format("%d 个待%s", ticked, word));
            line.acts = true;
            line.saved = saved;
            line.atLeast = atLeast;
            line.count = ticked;
            if (unknown > 0) {
                line.sub = unknown == ticked ? "正在计算可节省空间" : format("仍有 %d 个在计算", unknown);
            } else {
                const char* where = freesSpace ? "可释放空间"
                    : tab == TabFiles && installsGameFiles() ? (_settings.installToConsole ? "装在主机" : "装在 SD 卡")
                    : _settings.compress ? "压缩 NSP" : "NSP";
                line.sub = format("现在 %s，处理后 %s（%s）", formatSize(now).c_str(), formatSize(std::max<i64>(now - saved, 0)).c_str(), where);
            }
            if (known > 0 && saved > 0)
                line.share = (double)saved / (double)known;
            (freesSpace ? frees : smaller) += std::max<i64>(saved, 0);
            anyAtLeast = anyAtLeast || atLeast;
            std::stable_sort(savers.begin(), savers.end(), [](const Row* a, const Row* b) { return a->estimate.sureSavedBytes() > b->estimate.sureSavedBytes(); });
            for (size_t i = 0; i < savers.size() && i < 3; i++)
                card.biggest.push_back(OverviewGame{savers[i]->key, savers[i]->name, savers[i]->icon, savers[i]->tile, savers[i]->estimate.sureSavedBytes()});
            // The rest of what the line counts, so what's shown and what's more always add up to it.
            card.more = ticked - (int)card.biggest.size();
        } else if (toDo > 0) {
            add(OverviewLine::ToDo, format("%d 个游戏，未勾选", toDo), "打开列表勾选要处理的游戏");
        }
        // What its developer compressed, worth compressing again but not ticked, first of what's at the bottom, as it's to do: A opens the list,
        // where each says why it isn't ticked.
        if (developer > 0) {
            add(OverviewLine::Developer, developer == 1 ? std::string("1 个由开发者压缩，未勾选")
                                                        : format("%d 个由开发者压缩，未勾选", developer))
                .keys = developerKeys;
        }
        // Copies an older version left saying they need their update come first of what's at the bottom, as those games don't start at all.
        if (wontStart > 0)
            add(OverviewLine::WontStart, format("%d 个游戏当前无法启动", wontStart));
        // Thorough, how far reading every game in full has got. Quick, the games a first look can't vouch for, which stay as they are unless
        // they're ticked, and which Thorough would read.
        if (_settings.compress && (_settings.thorough ? unmeasured > 0 : notSure > 0)) {
            std::string text = _settings.thorough ? format("正在测量精确数值：剩余 %d 个", unmeasured)
                                                  : format("%d 个游戏可能更省：完整分析将逐个测量", notSure);
            OverviewLine& line = add(OverviewLine::NotSure, text);
            line.acts = true;
            line.count = _settings.thorough ? unmeasured : notSure;
            // Worked out again each time it's drawn, as it goes down while a game is read.
            if (_settings.thorough)
                line.sub = measureTimeLocked(tab);
        }
        if (!toInstall.empty()) {
            OverviewLine& line = add(OverviewLine::ToInstall, format("%d 个已压缩、未安装", (int)toInstall.size()));
            line.keys = toInstall;
            line.acts = true;
        }
        // Each game is counted once: one that can still be installed has its own line.
        done -= (int)toInstall.size();
        if (done > 0)
            add(OverviewLine::Done, freed > 0 && tab == TabInstalled ? format("已完成 %d 个，释放 %s", done, formatSize(freed).c_str()) : format("已完成 %d 个", done));
        if (card.lines.empty()) {
            // Looking everywhere, some folders are left out, which is worth knowing when a game isn't found.
            std::string sub = all > 0 || tab == TabInstalled ? ""
                            : "位于 " + whereGameFilesAre() + " 上的 NSP、NSZ、XCI、XCZ 文件会出现在这里。" +
                                  (_settings.filesFolder.empty() ? " 部分文件夹被排除，「搜索位置」可查看范围。" : "");
            OverviewLine& line = add(OverviewLine::Nothing, all > 0 ? "这里暂无可处理项" : tab == TabInstalled ? "没有已安装的游戏" : "没有找到游戏文件", sub);
            line.sources = tab == TabFiles && all == 0;
        }
        tickedAll += ticked;
        toDoAll += toDo;
        doneAll += done;
        wontStartAll += wontStart;
    }

    // The one number that matters most: space freed on the console, or else how much smaller game files come out.
    if (frees > 0)
        o.headline = (anyAtLeast ? "至少可释放 " : "可释放 ") + formatSize(frees);
    else if (smaller > 0)
        o.headline = (anyAtLeast ? "至少可节省 " : "可节省 ") + formatSize(smaller);
    else if (looking)
        o.headline = "正在查看你的游戏";
    else if (_analystRunning)
        o.headline = "正在计算可节省的空间";
    // With nothing to free, a game that won't start matters more than anything else that could be said.
    else if (wontStartAll > 0)
        o.headline = wontStartAll == 1 ? "1 个游戏当前无法启动" : format("%d 个游戏当前无法启动", wontStartAll);
    else if (tickedAll > 0)
        o.headline = tickedAll == 1 ? "1 个游戏就绪" : format("%d 个游戏就绪", tickedAll);
    else if (toDoAll > 0)
        o.headline = "未勾选任何游戏";
    else if (doneAll > 0)
        o.headline = "全部处理完毕";
    else
        o.headline = "暂时无事可做";

    // Where that space comes back, place by place. Game files never move these: new NSPs are written, and installing takes space.
    auto meter = [&](const char* label, i64 free, i64 total, i64 freed) {
        if (free >= 0 && total > 0)
            o.meters.push_back(OverviewMeter{label, free, total, freed});
    };
    meter("SD 卡", _freeSd, _totalSd, freedOnLocked(Storage::Sd));
    meter("主机内存", _freeNand, _totalNand, freedOnLocked(Storage::Nand));
    meter("USB 盘", _freeUsb, _totalUsb, 0);
    return o;
}

void App::openOverviewLine(const OverviewLine& line) {
    // The games a line is about, where it's a few that would be hard to find on a long list: the pointer goes to the first of them, whose
    // own line says why it's there.
    if (line.kind == OverviewLine::Developer && !line.keys.empty()) {
        openOverviewGame(line.tab, line.keys[0]);
        return;
    }
    showList(line.tab, line.kind == OverviewLine::Done || line.kind == OverviewLine::ToInstall ? View::Done : View::ToDo);
}

/// X does what X does on the list, to just what the line counts, without leaving the overview, which is where the app comes back to.
void App::actOnOverviewLine(const OverviewLine& line) {
    if (!line.acts)
        return;
    {
        sys::Lock lock(_mutex);
        View view = line.kind == OverviewLine::ToInstall ? View::Done : View::ToDo;
        if (_view[line.tab] != view) {
            _view[line.tab] = view;
            _focus[line.tab] = 0;
            _scroll[line.tab] = 0;
        }
        _tab = line.tab;
        _focusShown = _focus[_tab];
        _scrollShown = _scroll[_tab];
    }
    if (line.kind == OverviewLine::ToInstall)
        onlyThese(line.keys, [this] { pressInstall(); });
    else
        pressCompress();
}

int App::primaryLine(const OverviewCard& card) {
    for (size_t i = 0; i < card.lines.size(); i++) {
        if (card.lines[i].kind == OverviewLine::ToDo && card.lines[i].acts)
            return (int)i;
    }
    for (size_t i = 0; i < card.lines.size(); i++) {
        if (card.lines[i].kind == OverviewLine::ToInstall && card.lines[i].acts)
            return (int)i;
    }
    return -1;
}

std::string App::actLabel(const OverviewLine& line) const {
    bool installing = line.kind == OverviewLine::ToInstall || (line.tab == TabFiles && installsGameFiles());
    int count = line.kind == OverviewLine::ToInstall ? (int)line.keys.size() : line.count;
    return format("%s %d 个游戏", installing ? "安装" : _settings.compress ? "压缩" : "重打包", count);
}

/// A on one of the games under a line opens its list on what's to do, with the pointer on that game.
void App::openOverviewGame(int tab, const std::string& key) {
    showList(tab, View::ToDo);
    sys::Lock lock(_mutex);
    auto rows = visibleRowsLocked(tab);
    for (size_t i = 0; i < rows.size(); i++) {
        if (rows[i]->key != key)
            continue;
        _focus[tab] = (int)i;
        _focusSeen[tab] = (int)i;
        keepFocusVisible();
        _focusShown = _focus[tab];
        _scrollShown = _scroll[tab];
        _dirty = true;
        break;
    }
}

void App::pressOverviewStop(const OverviewStop& stop, const OverviewLine& line) {
    switch (stop.kind) {
        case OverviewStop::Act:
            actOnOverviewLine(line);
            break;
        case OverviewStop::Game:
            openOverviewGame(stop.tab, stop.key);
            break;
        case OverviewStop::Sources:
            openMenu(MenuSources);
            break;
        default:
            // A line of compressed files to install installs them, as it says. Choose, and the other lines, open the list on what they count.
            if (stop.kind == OverviewStop::Line && line.kind == OverviewLine::ToInstall)
                actOnOverviewLine(line);
            else if (stop.kind == OverviewStop::Line && line.kind == OverviewLine::NotSure)
                explainMeasuring(line.tab);
            else if (stop.kind == OverviewStop::Line && line.kind == OverviewLine::WontStart)
                askToFixMarks();
            else
                openOverviewLine(line);
            break;
    }
}

void App::pointOverviewAt(const OverviewStop& stop, const OverviewLine& line) {
    _overviewAt.tab = stop.tab;
    _overviewAt.kind = line.kind;
    _overviewAt.stop = stop.kind;
    _overviewAt.game = stop.key;
}

const char* App::viewName(View view) {
    switch (view) {
        case View::Done:
            return "已完成";
        case View::Everything:
            return "全部";
        default:
            return "待处理";
    }
}

std::string App::sortDescription() {
    switch (_settings.sort) {
        case 0:
            return _sortedByGain[_tab] ? "节省空间最多的优先" : "分析完成前按名称排序";
        case 1:
            return "体积大的优先";
        default:
            return "按名称";
    }
}

void App::changeSort(int direction) {
    _settings.sort = (_settings.sort + direction + 3) % 3;
    saveSettings();
    sys::Lock lock(_mutex);
    _focus[TabInstalled] = _focus[TabFiles] = 0;
    sortRowsLocked();
    _dirty = true;
}

void App::hideFocused() {
    sys::Lock lock(_mutex);
    auto rows = visibleRowsLocked(_tab);
    int index = _focus[_tab];
    if (index < 0 || index >= (int)rows.size())
        return;
    std::string key = rows[(size_t)index]->key;
    _hidden.insert(key);
    saveHiddenLocked();
    _rows.erase(std::remove_if(_rows.begin(), _rows.end(), [&](const Row& r) { return r.key == key; }), _rows.end());
    _focus[_tab] = std::max(0, std::min(index, (int)rows.size() - 2));
    _listChanged = true;
    _dirty = true;
}

void App::showHidden() {
    quietenWorker();
    {
        sys::Lock lock(_mutex);
        _hidden.clear();
        saveHiddenLocked();
    }
    _scanned[TabInstalled] = !_console;
    forgetFileScan();
}

/// The list of game files shows what's known of under where it now looks straight away, and looks through it all again.
void App::lookElsewhere() {
    forgetFileScan();
    std::vector<std::string> roots = fileRoots();
    sys::Lock lock(_mutex);
    showIndexRowsLocked(indexedPathsLocked(roots), false);
}

void App::chooseFilesFolder() {
    browse("游戏文件搜索范围", _settings.filesFolder, "搜索所有位置", [this](const std::string& path) {
        quietenWorker();
        _settings.filesFolder = path;
        saveSettings();
        lookElsewhere();
    });
}

/// Turns one of the places the list looks on or off, and looks again.
void App::toggleSource(bool& source) {
    quietenWorker();
    source = !source;
    saveSettings();
    lookElsewhere();
}

/// The way a PC on both is read changes for what's read next: connections made the other way are let go, and its games are listed again the
/// new way.
void App::toggleWifiFirst() {
    quietenWorker();
    _settings.wifiFirst = !_settings.wifiFirst;
    saveSettings();
    remote::setWifiFirst(_settings.wifiFirst);
    remote::forgetLinks();
    lookElsewhere();
}

/// Forgets what was worked out for what's ticked, or for everything if nothing is, so that it gets worked out again. A list being looked
/// through gives way first, as what it comes up with goes by what's known, and so does the analyst if it's working on one of these. A game
/// being read in full that isn't one of them goes on, and these get their first looks in the middle of it (firstLooksWhileMeasuring): giving
/// way, it would start over.
void App::analyzeAgain() {
    if (_jobKind == JobKind::Scanning) {
        _scanStop = true;
        _worker.join();
    }
    // What's ticked on the list, or all of it if nothing is, and whether anything was.
    auto chosenLocked = [this](bool& anyTicked) {
        auto rows = visibleRowsLocked(_tab);
        anyTicked = std::any_of(rows.begin(), rows.end(), [](const Row* row) { return row->ticked && row->blocked.empty(); });
        std::vector<Row*> chosen;
        for (auto* row : rows) {
            if (row->blocked.empty() && (!anyTicked || row->ticked))
                chosen.push_back(row);
        }
        return chosen;
    };
    bool anyTicked = false, workingOnOne = false;
    {
        sys::Lock lock(_mutex);
        for (auto* row : chosenLocked(anyTicked))
            workingOnOne = workingOnOne || row->state == RowState::Working;
    }
    if (workingOnOne)
        stopAnalyst();
    int count = 0;
    {
        sys::Lock lock(_mutex);
        for (auto* row : chosenLocked(anyTicked)) {
            _cache.erase(cacheKey(*row));
            _cache.erase(cacheKey(*row) + OneFileFailure);
            _previousAnswers.erase(cacheKey(*row));
            // It keeps its place until its new answer, rather than the list going by name until every game on it has one.
            if (row->hasEstimate)
                row->kept = Row::Kept{gainRank(*row), row->estimate.sureSavedFraction(), isCompressed(*row)};
            row->hasEstimate = false;
            row->belowMinimum.clear();
            row->byHand = false;
            if (row->state == RowState::Failed)
                row->state = RowState::Ready;
            row->message.clear();
            if (!anyTicked)
                row->ticked = row->state != RowState::Done;
            count++;
        }
        _cacheChanged = true;
        sortRowsLocked();
        _dirty = true;
    }
    _autoAnalyze[_tab] = true;
    showToast(count == 1 ? "正在重新分析它" : format("正在重新分析 %d 个游戏", count));
}

/// Whether the list on the screen is being looked through right now, which B stops.
bool App::lookingThroughThisList() const { return _jobKind == JobKind::Scanning && _scanningTab == _tab; }

/// A big USB drive, or a PC sharing a lot, can take a while to go through. What's been found by then stays, ready to work with.
void App::stopLooking() {
    if (!lookingThroughThisList())
        return;
    _scanStoppedByHand = true;
    _scanStop = true;
    _dirty = true;
}

void App::refreshList() {
    quietenWorker();
    if (_tab == TabInstalled) {
        _scanned[_tab] = !_console;
    } else {
        // Asking for it is asking for all of it to be looked at again, even what doesn't look like it's changed.
        std::vector<std::string> roots = fileRoots();
        {
            sys::Lock lock(_mutex);
            for (auto& item : _fileIndex) {
                if (isWithinAny(item.first, roots))
                    item.second.size = -1;
            }
        }
        forgetFileScan();
    }
    showToast("正在重新浏览列表");
}

// -------------------------------------------------------------------------------------------------------------------
// Jobs, and the questions before them

void App::stopAnalyzing() {
    _autoAnalyze[_tab] = false;
    _sink->stop = true;
}

void App::lookAgain() {
    quietenWorker();
    _scanned[TabInstalled] = !_console;
    forgetFileScan();
    showToast("正在重新查找游戏");
}

void App::stopMeasuring() {
    _autoAnalyze[TabInstalled] = _autoAnalyze[TabFiles] = false;
    _sink->stop = true;
}

void App::goOnMeasuring() { _autoAnalyze[TabInstalled] = _autoAnalyze[TabFiles] = true; }

bool App::stopAsked() const { return _sink->stop; }

/// Whoever wants to get on with it needn't wait: what's still unknown gets worked out as each game's turn comes, and a list coming up gives
/// way and picks up again afterwards. Only with nothing ticked yet does the list on the screen get to come up first.
void App::doWhenFree(std::function<void()> action) {
    bool toAnalyze;
    {
        sys::Lock lock(_mutex);
        toAnalyze = !keysToAnalyzeLocked(_tab).empty();
    }
    bool ready = canActNow();
    if (_jobKind == JobKind::Scanning && (ready || _scanningTab != _tab))
        _scanStop = true;
    // Analysis that was going on by itself only gives way, and goes on again afterwards. Reading a game in full goes on while what was asked
    // for is asked about, and only gives way once a job starts: stopped, that game starts over, and it may be most of the way through.
    bool measuring = _analystRunning && _analystMeasuring;
    if (_autoAnalyze[_tab] && !measuring && (_analystRunning || toAnalyze || !_scanned[_tab]))
        _analysisPausedTab = _tab;
    if (_analystRunning && !measuring)
        stopAnalyzing();
    else if (!measuring && (toAnalyze || !_scanned[_tab]))
        _autoAnalyze[_tab] = false;
    if ((!busy() || (measuring && !_workerRunning)) && ready) {
        confirmTicked();
        action();
    } else {
        _afterWork = std::move(action);
    }
}

/// Whether there's something to act on yet: the list on the screen has come up, or games on it are ticked already. Game files that are
/// only remembered from last time count, as what's left of them gets looked at before anything is done.
bool App::canActNow() {
    if (_scanned[_tab])
        return true;
    sys::Lock lock(_mutex);
    for (auto* row : visibleRowsLocked(_tab)) {
        if (row->ticked && row->blocked.empty() && (row->ready || row->tab == TabFiles))
            return true;
    }
    return false;
}

/// A list of game files that gave way part way through shows some of them as they were last time. Those that are ticked can be acted on all
/// the same, as long as all their files are still there.
void App::confirmTicked() {
    if (_scanned[_tab] || _tab != TabFiles)
        return;
    std::vector<std::pair<std::string, std::vector<std::string>>> toCheck;
    {
        sys::Lock lock(_mutex);
        for (auto* row : visibleRowsLocked(_tab)) {
            if (!row->ticked || !row->blocked.empty() || row->ready)
                continue;
            std::vector<std::string> files = row->dlcFiles;
            for (auto* file : {&row->mainFile, &row->updateFile, &row->extracted}) {
                if (!file->empty())
                    files.push_back(*file);
            }
            toCheck.emplace_back(row->key, std::move(files));
        }
    }
    for (auto& [key, files] : toCheck) {
        bool there = !files.empty();
        for (auto& file : files) {
            try {
                there = there && sys::exists(file);
            } catch (const ToolError&) {
                there = false;
            }
        }
        if (!there)
            continue;
        sys::Lock lock(_mutex);
        for (auto& row : _rows) {
            if (row.tab == TabFiles && row.key == key)
                row.ready = true;
        }
    }
}

void App::pressCompress() {
    _installNow = false;
    _keepGamesWithoutUpdates = false;
    doWhenFree([this] { askToCompress(); });
}

void App::pressInstall() {
    _installNow = true;
    _keepGamesWithoutUpdates = false;
    doWhenFree([this] { askToCompress(); });
}

void App::pressQuit() {
    // Just looking to see if there's a newer version isn't anything to bring up, only getting one is.
    bool updating;
    {
        sys::Lock lock(_mutex);
        updating = _updating && _updateState.rfind("Getting", 0) == 0;
    }
    bool analyzing = _analystRunning;
    auto dialog = std::make_unique<Dialog>();
    dialog->title = "要退出 hacBrewPackLZ 吗？";
    if (analyzing)
        dialog->paragraphs.push_back("仍在执行分析，已得出的结果会保留，剩余部分下次继续。");
    else if (updating)
        dialog->paragraphs.push_back("正在自我更新，更新会被中止，下次启动时重试。");
    dialog->choices.push_back({analyzing || updating ? "继续" : "留下", nullptr});
    dialog->choices.push_back({"退出", [this] { _quit = true; }});
    dialog->cancelChoice = 0;
    // B and then A to leave, and B twice comes back to where it was.
    dialog->focus = 1;
    _dialog = std::move(dialog);
    _dirty = true;
}

void App::beginJob(JobKind kind, const std::vector<std::string>& keys, std::function<void()> work) {
    prepareJob(keys, kind);
    _screen = Screen::Job;
    runInBackground(kind, std::move(work));
}

void App::beginCompress(const std::vector<std::string>& keys) {
    if (!keys.empty())
        beginJob(JobKind::Compressing, keys, [this, keys] { compress(keys); });
}

void App::beginCheck() {
    if (_workerRunning)
        return;
    std::vector<std::string> keys = tickedGames();
    if (keys.empty())
        showMessage("未勾选任何游戏", "在要校验的游戏上按 A 键，然后重试。");
    else
        beginJob(JobKind::Checking, keys, [this, keys] { check(keys); });
}

void App::beginExtract() {
    if (_workerRunning)
        return;
    std::vector<std::string> keys = tickedGames();
    if (keys.empty())
        showMessage("未勾选任何游戏", "在要解包的游戏上按 A 键，然后重试。");
    else
        beginJob(JobKind::Extracting, keys, [this, keys] { extract(keys); });
}

/// With compression off, a game only comes out any smaller by having an update made part of it, so there's nothing in it for the others. They
/// may well be ticked for something else, so nothing happens to them without asking. True if it has asked.
bool App::askAboutGamesWithoutUpdates() {
    if (_settings.compress || _keepGamesWithoutUpdates)
        return false;
    // A compressed game doesn't come out the same size but bigger, uncompressed, and is only ticked by someone who wants that.
    auto sameSize = [](const Row& row) {
        bool grows = row.compressedFile || (row.hasEstimate && row.estimate.savedFraction() < -NothingToGain);
        return row.ticked && row.blocked.empty() && !row.hasUpdate && row.extracted.empty() && !grows;
    };
    std::vector<std::string> names;
    {
        sys::Lock lock(_mutex);
        for (auto* row : visibleRowsLocked(_tab)) {
            bool beingInstalled = row->tab != TabInstalled && goesOnConsole(*row);
            if (sameSize(*row) && !beingInstalled)
                names.push_back(row->name);
        }
    }
    if (names.empty())
        return false;

    auto dialog = std::make_unique<Dialog>();
    bool one = names.size() == 1;
    dialog->title = one ? "此游戏没有更新补丁" : format("%d 个游戏没有更新补丁", (int)names.size());
    dialog->paragraphs.push_back(std::string("关闭压缩时，重打包只能通过把更新补丁并入本体来缩小体积。 ") + (one ? "这个处理后体积不变，白费时间，可以取消勾选。" : "这些处理后体积不变，白费时间，可以取消勾选。"));
    dialog->paragraphs.push_back(someNames(names, 3) + ".");
    dialog->choices.push_back({one ? "取消勾选它" : "取消勾选它们", [this, sameSize] {
        bool anyLeft = false;
        {
            sys::Lock lock(_mutex);
            for (auto* row : visibleRowsLocked(_tab)) {
                if (sameSize(*row)) {
                    row->ticked = false;
                    row->byHand = true;
                }
                anyLeft = anyLeft || (row->ticked && row->blocked.empty());
            }
            _dirty = true;
        }
        if (anyLeft)
            askToCompress();
        else
            showMessage("没有可重打包的游戏", "已勾选的游戏中，没有一个带有可并入的更新补丁。");
    }});
    dialog->choices.push_back({one ? "仍重打包它" : "仍一并重打包", [this] {
        _keepGamesWithoutUpdates = true;
        askToCompress();
    }});
    dialog->choices.push_back({"取消", nullptr});
    dialog->cancelChoice = 2;
    _dialog = std::move(dialog);
    _dirty = true;
    return true;
}

std::vector<std::string> App::notSureKeysLocked(int tab, bool readyOnly) {
    std::vector<const Row*> rows;
    for (auto& row : _rows) {
        if (readyOnly && row.state != RowState::Ready)
            continue;
        // A copy an older version made waits under Done, and is measured all the same.
        if (row.tab == tab && row.hasEstimate && row.notSure && row.blocked.empty() && (inViewLocked(row, View::ToDo) || (row.estimate.olderCopy && inViewLocked(row, View::Done))))
            rows.push_back(&row);
    }
    // The nearer a first look puts one to being worth it, the likelier reading all of it finds it is, so stopping part way leaves the least behind.
    std::stable_sort(rows.begin(), rows.end(), [](const Row* a, const Row* b) { return a->estimate.savedFraction() > b->estimate.savedFraction(); });
    std::vector<std::string> keys;
    for (auto* row : rows)
        keys.push_back(row->key);
    return keys;
}

/// Whether reading a game in full would say more than its first look did: not once it's measured, compressed already, or sure to grow, which
/// is a fact about the file, nor what isn't to be done at all.
bool App::unmeasuredLocked(const Row& row) {
    // The one being read in full counts too, until it's done. One under 1 GB measured before those were measured exactly is read once more.
    if (!row.hasEstimate || (row.estimate.measured && !row.measureAgain) || row.estimate.alreadyCompressed || !row.blocked.empty() || !row.ready ||
        (row.state != RowState::Ready && row.state != RowState::Working))
        return false;
    // Growing on a first look's say is no fact: such games, compressed, came to a few kilobytes less, so they're measured like any other. An
    // NSZ made into a file, compared with that file, does grow.
    if (row.estimate.sureToGrow(_settings.compress))
        return false;
    // Nor is one that reading in full can need a temporary copy on the SD card for: a number isn't worth wearing the card for.
    if (temporaryCopy(row) > 0)
        return false;
    // Wherever it's shown: what's under Done, such as a copy an older version made or one its developer compressed, is measured all the same.
    return inViewLocked(row, View::Everything);
}

std::string App::nothingToGainReason(const Row& row, const SizeEstimate& estimate) {
    if (row.tab != TabInstalled || !estimate.alreadyCompressed)
        return "";
    // One its developer compressed is worked out like any other game now, so this is only one remembered from before.
    if (estimate.byMakers)
        return "由开发者压缩：无收益";
    return estimate.freedBytes > 0 ? "已压缩：无收益" : "无可压缩的游戏数据";
}

/// What's compressed already by this version, or has no game data to compress, has nothing to gain whoever ticked it. One ticked by itself
/// that's settled as not worth it is left out too, as the job would leave it out. One ticked by hand otherwise is done whatever it saves.
std::string App::leftAsItIsLocked(const Row& row) {
    if (!_settings.compress || !row.hasEstimate)
        return "";
    std::string why = nothingToGainReason(row, row.estimate);
    if (why.empty() && !row.byHand && !_installNow && !row.notSure && !row.belowMinimum.empty() && !compressedToInstallLocked(row))
        why = row.belowMinimum;
    return why;
}

bool App::firstLooksLeftLocked() {
    for (int tab = 0; tab < TabCount; tab++) {
        if (_scanningTab == tab || (!_scanned[tab] && (_scanWanted[tab] || _overview)) || (_scanned[tab] && _autoAnalyze[tab] && !keysToAnalyzeLocked(tab).empty()))
            return true;
    }
    return false;
}

std::vector<std::string> App::measureKeysLocked(int tab) {
    if (!_settings.compress)
        return {};
    // The games it could change the answer for first, then the rest, the smallest first in each: more of them get their exact figures sooner,
    // and what gives way part way through, which starts that game over, costs the least.
    std::vector<const Row*> doubtful, rest;
    for (auto& key : notSureKeysLocked(tab, true)) {
        if (const Row* row = findLocked(key))
            doubtful.push_back(row);
    }
    for (auto& row : _rows) {
        if (row.tab == tab && !row.notSure && row.state == RowState::Ready && unmeasuredLocked(row))
            rest.push_back(&row);
    }
    auto bytes = [](const Row* row) { return row->estimate.romFsBytes > 0 ? row->estimate.romFsBytes : row->now; };
    auto smallestFirst = [&](const Row* a, const Row* b) { return bytes(a) < bytes(b); };
    std::stable_sort(doubtful.begin(), doubtful.end(), smallestFirst);
    std::stable_sort(rest.begin(), rest.end(), smallestFirst);
    std::vector<std::string> keys;
    for (auto* row : doubtful)
        keys.push_back(row->key);
    for (auto* row : rest)
        keys.push_back(row->key);
    return keys;
}

/// What the line about measuring on a list's card is: what goes on, what it has left, and how it's turned off or on, which is where that's
/// found, when it matters.
void App::explainMeasuring(int tab) {
    int left = 0, doubtful = 0;
    i64 bytes = 0;
    std::string time;
    {
        sys::Lock lock(_mutex);
        for (auto& row : _rows) {
            if (row.tab != tab || !unmeasuredLocked(row))
                continue;
            left++;
            doubtful += row.notSure ? 1 : 0;
            bytes += row.measureBytes();
        }
        time = measureTimeLocked(tab);
    }
    auto dialog = std::make_unique<Dialog>();
    if (_settings.thorough) {
        dialog->title = "正在测量精确数值";
        dialog->paragraphs.push_back("初览每个游戏很快，但只有通读全部内容才能算出可节省多少：1 GB 以下的游戏可精确到字节，"
                                     "更大的游戏误差不到百分之一。程序打开期间会自动进行，优先处理可能改变结论的游戏，"
                                     "并勾选值得处理的游戏；你启动任何任务时都会让路，且只写入分析结果。");
        if (left > 0)
            dialog->paragraphs.push_back(format("本列表还剩 %d 个游戏、%s 游戏数据，按当前读取速度约需 %s。", left,
                                                formatSize(bytes).c_str(), time.c_str()));
        dialog->paragraphs.push_back("关闭后，初览无法确认的游戏保持未勾选且原样保留，除非你手动勾选。可在"
                                     "设置 → 分析中重新开启。");
        dialog->choices.push_back({"继续测量", nullptr});
        dialog->choices.push_back({"关闭该功能", [this] {
            _settings.thorough = false;
            saveSettings();
            if (_analystRunning && _analystStartedMeasuring)
                stopAnalyst();
            _dirty = true;
        }});
        dialog->cancelChoice = 0;
    } else {
        bool one = doubtful == 1;
        dialog->title = one ? "某个游戏可能更省空间" : "这些游戏可能更省空间";
        dialog->paragraphs.push_back(one ? "初览无法判断这个游戏能节省多少，因此未勾选，压缩时保持原样。手动勾选后，无论节省多少都会"
                                           "进行压缩。"
                                         : "初览无法判断这些游戏能节省多少，因此未勾选，压缩时保持原样。手动"
                                           "勾选后，无论节省多少都会压缩。");
        dialog->paragraphs.push_back("完整分析会在程序打开期间自动通读每个游戏，不写入任何内容，并勾选值得处理的游戏。它位于"
                                     "设置 → 分析。");
        dialog->choices.push_back({"Turn it on", [this] {
            _settings.thorough = true;
            saveSettings();
            _dirty = true;
        }});
        dialog->choices.push_back({"确定", nullptr});
        dialog->cancelChoice = 1;
    }
    _dialog = std::move(dialog);
    _dirty = true;
}

/// Said after what more needs to be free, when that's for a temporary copy as well as the game.
static const char* const CopyCounted = " 其中包含处理紧凑游戏时临时副本所需的空间。";

void App::askToCompress(bool toConsole) {
    if (_workerRunning)
        return;
    if (askAboutGamesWithoutUpdates())
        return;

    int count = 0, unknown = 0, extracted = 0, onConsole = 0, toFile = 0, developer = 0;
    i64 saved = 0;
    bool atLeast = false;
    // What's ticked and left as it is all the same, said here rather than found out at the end, and what its developer compressed, which
    // compressed again here needs its original to be updated.
    std::vector<std::string> leftNames;
    bool nothingToGain = true;
    std::string leftWhy, developerName;
    {
        sys::Lock lock(_mutex);
        for (auto* row : visibleRowsLocked(_tab)) {
            if (!row->ticked || !row->blocked.empty())
                continue;
            std::string why = leftAsItIsLocked(*row);
            if (!why.empty()) {
                leftNames.push_back(row->name);
                leftWhy = why;
                nothingToGain = nothingToGain && !row->underMinimum;
                continue;
            }
            count++;
            extracted += row->extracted.empty() ? 0 : 1;
            if (goesOnConsole(*row))
                onConsole++;
            else
                toFile++;
            if (row->hasEstimate) {
                saved += row->sureSaved();
                atLeast = atLeast || row->estimate.atLeast();
                // Repacked, it's unpacked, and its developer's packing is gone just the same. A game file installed with compression off goes
                // in as it is, packing and all.
                bool asItIs = !_settings.compress && row->tab == TabFiles && goesOnConsole(*row);
                if (row->estimate.byMakers && !row->estimate.alreadyCompressed && !asItIs) {
                    developer++;
                    developerName = row->name;
                }
            } else {
                unknown++;
            }
        }
    }
    bool replacing = _tab == TabInstalled && replacesInstalled();
    bool installing = _tab != TabInstalled && onConsole > 0;
    std::string word = installing ? "安装" : verb();
    std::string lower = word;
    lower[0] = (char)tolower((u8)lower[0]);
    std::string leftSaid = leftNames.empty() ? ""
                         : leftNames.size() == 1 ? format("%s 保持原样。%s。", leftNames[0].c_str(), leftWhy.c_str())
                         : someNames(leftNames, 3) + (nothingToGain ? " 保持原样：没有收益。" : " 保持原样，列表中各有说明。");
    if (count == 0) {
        if (!leftNames.empty())
            showMessage("Nothing to " + lower, leftSaid);
        else
            showMessage("未勾选任何游戏", "在要处理的游戏上按 A 键以" + lower + "，然后重试。");
        return;
    }

    if (onConsole > 0 && _settings.compress && !_platform.firmwareReadsCompression()) {
        showMessage("当前系统版本过旧", "压缩游戏需要 12.0.0 或更高系统版本；仍可保存为 NSP，请在设置中选择。");
        return;
    }

    // Better said now than found out part of the way through.
    std::string folder = nspFolder();
    if (toFile > 0) {
        sys::makeDirectories(folder);
        if (!sys::isDirectory(folder)) {
            showMessage("无法访问该文件夹", "NSP 设置为存放到 " + folder + "，但该位置不存在。如果在 USB 盘上，请插好；也可以在设置里另选文件夹。");
            return;
        }
    }
    _consoleOnce = toConsole;
    RoomPlan plan = planRoom();
    _consoleOnce = false;
    // The SD card hasn't room for some of them, and the console has: they can go there instead, this time.
    if (installing && !toConsole && !_settings.installToConsole && !plan.wontFit.empty() && plan.reason == "SD 卡") {
        _consoleOnce = true;
        RoomPlan there = planRoom();
        _consoleOnce = false;
        if (there.wontFit.size() < plan.wontFit.size()) {
            auto offer = std::make_unique<Dialog>();
            offer->title = "改为安装到主机？";
            offer->paragraphs.push_back(format("%s：SD 卡缺少 %s，主机空间足够。", someNames(plan.wontFit, 3).c_str(),
                                               formatSize(plan.mostNeeded).c_str()));
            offer->paragraphs.push_back("仅对本次生效；游戏安装位置由「设置 → 安装游戏到」决定。");
            offer->choices.push_back({"安装到主机", [this] { askToCompress(true); }});
            offer->choices.push_back({"暂不", nullptr});
            offer->cancelChoice = 1;
            _dialog = std::move(offer);
            _dirty = true;
            return;
        }
    }
    std::string notFitting = someNames(plan.wontFit, 3);
    bool tooLarge = plan.reason.find("can't hold") != std::string::npos;
    int going = (int)plan.order.size();
    if (going == 0) {
        if (tooLarge)
            showMessage("空间不足，无法容纳", notFitting + ": " + plan.reason + "。FAT32 单文件上限为 4 GB，需要改用 exFAT 或 NTFS。");
        else
            showMessage("空间不足", format("%s：缺少 %s %s空间。%s", notFitting.c_str(), formatSize(plan.mostNeeded).c_str(), plan.reason.c_str(),
                                                          plan.countsCopy ? CopyCounted : ""));
        return;
    }

    auto dialog = std::make_unique<Dialog>();
    dialog->title = going == 1 ? word + " 这个游戏？" : format("%s这 %d 个游戏？", word.c_str(), going);
    auto say = [&](const std::string& paragraph) { dialog->paragraphs.push_back(paragraph); };
    if (saved > 0 && replacing)
        say(format(atLeast ? (unknown > 0 ? "这至少会释放 %s，以及尚未分析的游戏可能节省的空间。" : "这至少会释放 %s。")
                          : (unknown > 0 ? "这会释放 %s，以及尚未分析的游戏可能节省的空间。" : "这会释放 %s。"),
                   formatSize(saved).c_str()));
    if (replacing) {
        // What happens to each, and what to know about what it becomes: two short paragraphs rather than a page.
        say(format("每个游戏都会在新副本写入并校验通过后，替换为%s副本；存档不受影响。", _settings.compress ? "压缩" : "重打包"));
        say("成品需要签名补丁才能启动，更新补丁也需在此整合。");
        if (!_settings.warned)
            say("首次在本机使用？建议先试一个小游戏。");
    } else if (_tab == TabInstalled) {
        say("已安装的游戏保持原样，不会释放空间；每个游戏的 NSP 会保存到 " + folder + ".");
    } else if (installing) {
        std::string then = _settings.deleteInstalled ? "安装完成后，该文件会被删除。" : "文件本身不会被修改。";
        std::string where = toConsole || _settings.installToConsole ? "主机上" : "SD 卡上";
        say((_settings.compress ? "每个游戏都会先压缩（已压缩的除外），然后安装到" + where : "每个游戏都会原样安装到" + where) + "，并替换该游戏的旧版本。 " + then + " 以此方式存储的游戏需要 sigpatches 才能启动。");
    } else if (extracted < count) {
        say(std::string(_settings.compress ? "一个压缩 NSP" : "一个 NSP") + " 会保存到 " + folder + "。文件本身不会被修改。");
    }
    if (extracted > 0 && !installing)
        say(std::string(extracted == 1 ? "已解包游戏的 NSP" : "每个已解包游戏的 NSP") + " 会保存到 " + folder + "，原文件夹不变。");
    if (!notFitting.empty())
        say(notFitting + (tooLarge ? "：" + plan.reason + "，因此已跳过。"
                                   : format("：已跳过，缺少 %s %s空间。%s", formatSize(plan.mostNeeded).c_str(), plan.reason.c_str(), plan.countsCopy ? CopyCounted : "")));
    if (!leftSaid.empty())
        say(leftSaid);
    // A solid NSZ with more to keep aside than there's memory for can take a temporary copy on the SD card, which is said now, as it wears the card.
    std::vector<std::string> copyNames;
    i64 mostCopy = 0;
    {
        sys::Lock lock(_mutex);
        for (auto& key : plan.order) {
            const Row* row = findLocked(key);
            i64 copy = row ? temporaryCopy(*row) : 0;
            if (copy > 0) {
                copyNames.push_back(row->name);
                mostCopy = std::max(mostCopy, copy);
            }
        }
    }
    if (copyNames.size() == 1)
        say(format("%s 打包得很紧，处理时可能需要在 SD 卡上建立最多 %s 的临时副本，完成后会删除。", copyNames[0].c_str(),
                   formatSize(mostCopy).c_str()));
    else if (copyNames.size() > 1)
        say(format("%s 打包得很紧，每个处理时都可能需要 SD 卡上最多 %s 的临时副本，完成后各自删除。",
                   someNames(copyNames, 3).c_str(), formatSize(mostCopy).c_str()));
    // An official update patches the bytes its developer stored, which compressing again here doesn't keep, and which can't be made again.
    const char* again = _settings.compress ? "再次压缩" : "重打包";
    if (developer == 1)
        say(developerName + format(" 由开发者压缩。在此%s后，更新需要原始游戏文件。", again));
    else if (developer > 1)
        say(format("其中 %d 个由开发者压缩。在此%s后，更新需要原始游戏文件。", developer, again));
    // Only what's ticked is done. Thorough, the games a first look can't vouch for that are still to be read in full may be ticked afterwards,
    // which is worth knowing before pressing it, unless they were unticked by hand. Not when what's ticked is to be installed as it is.
    int toMeasure = 0;
    if (_settings.compress && _settings.thorough && !_installNow) {
        sys::Lock lock(_mutex);
        for (auto& row : _rows)
            toMeasure += row.tab == _tab && row.notSure && !row.ticked && !row.byHand && unmeasuredLocked(row) ? 1 : 0;
    }
    if (toMeasure == 1)
        say("另有 1 个游戏可能更省空间，尚待测量：值得处理时会自动勾选。");
    else if (toMeasure > 1)
        say(format("另有 %d 个游戏可能更省空间，尚待测量：值得处理的会自动勾选。", toMeasure));
    if (_memoryLimited)
        say("本次从相册启动，可用内存很少。处理大型游戏时，请启动任意游戏并按住 R 打开自制程序菜单，从那里启动本程序。");
    int battery = _platform.batteryPercent();
    if (battery >= 0 && battery < 30 && !_platform.isCharging())
        say(format("当前电量 %d%%，建议接上电源，处理可能需要一段时间。", battery));

    std::vector<std::string> keys = plan.order;
    dialog->choices.push_back({word, [this, keys, toConsole] {
        if (!_settings.warned) {
            _settings.warned = true;
            saveSettings();
        }
        _consoleOnce = toConsole;
        beginCompress(keys);
    }});
    dialog->choices.push_back({"暂不", nullptr});
    dialog->cancelChoice = 1;
    _dialog = std::move(dialog);
    _dirty = true;
}

void App::askToStop() {
    int left;
    {
        sys::Lock lock(_mutex);
        left = _jobCount - _jobIndex;
    }
    auto dialog = std::make_unique<Dialog>();
    dialog->title = "停止？";
    dialog->paragraphs = {"正在处理的游戏保持原样，已完成的游戏也维持现状。"};
    dialog->choices.push_back({"继续", nullptr});
    if (left > 0 && !_stopAfterGame)
        dialog->choices.push_back({"完成本游戏后", [this] { _stopAfterGame = true; }});
    dialog->choices.push_back({"立即停止", [this] { _sink->stop = true; }});
    dialog->cancelChoice = 0;
    _dialog = std::move(dialog);
    _dirty = true;
}

void App::askToRemove(bool allTicked) {
    std::vector<std::string> keys, names, others;
    bool installed = false, anyFolder = false, anyFile = false;
    i64 bytes = 0, rowFileBytes = 0;
    {
        sys::Lock lock(_mutex);
        auto rows = visibleRowsLocked(_tab);
        int index = _focus[_tab];
        auto filesOf = [](const Row& row) {
            std::vector<std::string> files{row.mainFile};
            if (!row.updateFile.empty())
                files.push_back(row.updateFile);
            files.insert(files.end(), row.dlcFiles.begin(), row.dlcFiles.end());
            return files;
        };
        std::set<std::string> files;
        for (int i = 0; i < (int)rows.size(); i++) {
            const Row& row = *rows[(size_t)i];
            if (allTicked ? !(row.ticked && row.blocked.empty()) : i != index)
                continue;
            keys.push_back(row.key);
            names.push_back(row.name);
            installed = installed || row.tab == TabInstalled;
            anyFolder = anyFolder || !row.extracted.empty();
            bool file = row.tab != TabInstalled && row.extracted.empty();
            anyFile = anyFile || file;
            if (file) {
                rowFileBytes += row.now;
                for (auto& path : filesOf(row))
                    files.insert(path);
            } else {
                bytes += row.now;
            }
        }
        // A file can hold more than one game, each a row of its own, and deleting it takes them all: they're named, and what goes is the files.
        bool sized = !files.empty();
        i64 fileBytes = 0;
        for (auto& path : files) {
            auto known = _fileIndex.find(path);
            sized = sized && known != _fileIndex.end() && known->second.size >= 0;
            fileBytes += sized ? known->second.size : 0;
        }
        bytes += sized ? fileBytes : rowFileBytes;
        for (auto& row : _rows) {
            if (row.tab != TabFiles || !row.extracted.empty() || std::find(keys.begin(), keys.end(), row.key) != keys.end())
                continue;
            auto rowFiles = filesOf(row);
            if (std::any_of(rowFiles.begin(), rowFiles.end(), [&](const std::string& path) { return files.count(path) > 0; }))
                others.push_back(row.name);
        }
    }
    if (keys.empty())
        return;
    bool one = keys.size() == 1;
    std::string what = installed ? "卸载" : "删除";
    std::string things = installed ? "个游戏" : anyFolder && anyFile ? "个项目" : anyFolder ? "个文件夹" : "个文件";
    auto dialog = std::make_unique<Dialog>();
    dialog->title = one ? what + " " + names[0] + "？" : format("%s %d %s？", what.c_str(), (int)keys.size(), things.c_str());
    // Most of a list can be ticked without anyone having ticked it, so it says exactly what's about to go.
    if (!one)
        dialog->paragraphs.push_back(someNames(names, 6) + ".");
    if (!others.empty())
        dialog->paragraphs.push_back(std::string(one && !anyFolder ? "其文件还包含 " : "这些文件还包含 ") + someNames(others, 6) + "，这些内容也会一并处理。");
    dialog->paragraphs.push_back(installed ? format("这会从主机移除 %s（含更新补丁和 DLC），存档保留。", formatSize(bytes).c_str()) : format("这会删除 %s，且无法撤销。", formatSize(bytes).c_str()));
    dialog->choices.push_back({one ? "保留文件" : "保留它们", nullptr});
    dialog->choices.push_back({what, [this, keys] {
        doWhenFree([this, keys] {
            beginJob(JobKind::Removing, keys, [this, keys] { remove(keys); });
            _autoAnalyze[_tab] = true;
        });
    }});
    dialog->cancelChoice = 0;
    dialog->focus = 0;
    _dialog = std::move(dialog);
    _dirty = true;
}

/// The copies an older version left saying they need their update: why they don't start, and that fixing them changes nothing else.
void App::askToFixMarks() {
    std::vector<std::string> keys, names;
    int asksSystem = 0;
    u32 asked = 0;
    {
        sys::Lock lock(_mutex);
        for (auto& row : _rows) {
            if (row.tab == TabInstalled && row.markedIncomplete && row.blocked.empty() && row.state != RowState::Working) {
                keys.push_back(row.key);
                names.push_back(row.name);
                if (row.asksSystem) {
                    asksSystem++;
                    asked = std::max(asked, row.asksSystem);
                }
            }
        }
    }
    if (keys.empty())
        return;
    bool one = keys.size() == 1;
    int marked = (int)keys.size() - asksSystem;
    auto dialog = std::make_unique<Dialog>();
    dialog->title = one ? names[0] + " 当前无法启动" : format("%d 个游戏当前无法启动", (int)keys.size());
    std::string console = versionName(_platform.systemVersion());
    if (asksSystem > 0) {
        // Said for one, and for several each say what they ask for in their own row, so it's the newest the dialog names.
        dialog->paragraphs.push_back(
            one ? format("此副本要求系统版本 %s，而本机为 %s，因此主机会要求系统更新并拒绝启动。"
                         "当初的安装器已让它能在本机启动，生成副本时又从游戏文件里取回了版本要求。",
                         versionName(asked).c_str(), console.c_str())
                : asksSystem == (int)keys.size()
                ? format("这些副本要求的系统版本高于本机的 %s（最高 %s），因此主机会要求系统更新并拒绝"
                         "启动。当初的安装器已让它们能在本机启动，生成副本时又从"
                         "游戏文件里取回了版本要求。", console.c_str(), versionName(asked).c_str())
                : format("其中 %d 个副本要求的系统版本高于本机的 %s（最高 %s），因此主机会要求系统更新。"
                         "当初的安装器已让它们能在本机启动，生成副本时又从游戏文件里取回了版本要求。",
                         asksSystem, console.c_str(), versionName(asked).c_str()));
    }
    if (marked > 0) {
        dialog->paragraphs.push_back(
            one ? std::string("1.4.3 及更早版本从「本体＋更新」游戏生成此副本时，保留了需要更新的标记。副本本身完整，但"
                              "主机只认这个标记：会要求更新并拒绝启动。")
                : marked == (int)keys.size()
                ? std::string("1.4.3 及更早版本从「本体＋更新」游戏生成这些副本时，保留了需要更新的标记。副本本身完整，但"
                              "主机只认这个标记：会要求更新并拒绝启动。")
                : format("其中 %d 个由 1.4.3 及更早版本从「本体＋更新」游戏生成，保留了需要更新的标记：主机"
                         "会要求更新并拒绝启动。", marked));
    }
    if (!one)
        dialog->paragraphs.push_back(someNames(names, 6) + ".");
    dialog->paragraphs.push_back(one ? "「修复」会立即改正主机中该游戏的记录，其他内容不变。"
                                     : "「修复」会立即改正主机中每个游戏的记录，其他内容不变。");
    dialog->choices.push_back({"暂不", nullptr});
    dialog->choices.push_back({one ? "Fix it" : "Fix them", [this, keys] {
        // As with what the overview's buttons do, the list it's done on is the one behind it.
        {
            sys::Lock lock(_mutex);
            _tab = TabInstalled;
            _focusShown = _focus[_tab];
            _scrollShown = _scroll[_tab];
        }
        doWhenFree([this, keys] { beginJob(JobKind::Fixing, keys, [this, keys] { fixMarks(keys); }); });
    }});
    dialog->cancelChoice = 0;
    dialog->focus = 1;
    _dialog = std::move(dialog);
    _dirty = true;
}

// -------------------------------------------------------------------------------------------------------------------
// The + menu

/// What the menu needs to know about the list, to offer only what makes sense for it.
struct App::MenuFacts {
    size_t hidden = 0;
    size_t toAnalyze = 0;
    int rows = 0;
    int ticked = 0;
    bool anyFile = false;
    /// What removing the selected game, or the ticked ones, is called here.
    std::string removeThis;
    std::string removeTicked;
    std::string aboutName;
    std::vector<std::string> about;
    /// It can be opened, for its details and to look inside, which say what About this game did and more.
    bool canLook = false;
};

App::MenuFacts App::menuFacts() {
    MenuFacts facts;
    sys::Lock lock(_mutex);
    facts.hidden = _hidden.size();
    facts.toAnalyze = keysToAnalyzeLocked(_tab).size();
    // Thorough, what's still to be read in full is analyzing that's left too.
    if (_settings.thorough && _settings.compress)
        facts.toAnalyze += measureKeysLocked(_tab).size();
    bool tickedFolder = false, tickedFile = false;
    auto shown = visibleRowsLocked(_tab);
    for (auto* row : shown) {
        facts.rows++;
        bool isTicked = row->ticked && row->blocked.empty();
        facts.ticked += isTicked ? 1 : 0;
        facts.anyFile = facts.anyFile || row->extracted.empty();
        tickedFolder = tickedFolder || (isTicked && !row->extracted.empty());
        tickedFile = tickedFile || (isTicked && row->extracted.empty());
    }
    int index = _focus[_tab];
    if (index >= 0 && index < (int)shown.size()) {
        const Row& focused = *shown[(size_t)index];
        facts.about = focused.notes;
        // What stops it comes first, said in full, as the list only has room for the start of it.
        if (!focused.blocked.empty())
            facts.about.insert(facts.about.begin(), focused.blocked);
        else if (focused.state == RowState::Failed && !focused.message.empty())
            facts.about.insert(facts.about.begin(), focused.message);
        else if (focused.updateOverCopy && focused.state != RowState::Done)
            facts.about.insert(facts.about.begin(), UpdateToMakePartAbout);
        facts.aboutName = focused.name;
        facts.canLook = canLookInside(focused);
        if (focused.state != RowState::Working)
            facts.removeThis = focused.tab == TabInstalled ? (_console ? "卸载此游戏" : "") : !focused.extracted.empty() ? "删除此文件夹" : "删除此文件";
    }
    if (_tab == TabInstalled)
        facts.removeTicked = _console ? "卸载已勾选的游戏" : "";
    else
        facts.removeTicked = tickedFolder && tickedFile ? "删除已勾选的文件和文件夹" : tickedFolder ? "删除已勾选的文件夹" : "删除已勾选的文件";
    return facts;
}

void App::openMenu(int page, int root) {
    if (root < 0)
        root = page;
    MenuFacts facts = menuFacts();
    auto menu = std::make_unique<Menu>();
    auto add = [&](const std::string& label, std::function<void()> action) {
        Menu::Item item;
        item.label = label;
        item.action = std::move(action);
        menu->items.push_back(std::move(item));
    };
    auto addSetting = [&](const std::string& label, std::function<std::string()> value, std::function<void(int)> change) {
        Menu::Item item;
        item.label = label;
        item.value = std::move(value);
        item.change = std::move(change);
        menu->items.push_back(std::move(item));
    };
    auto addPage = [&](const std::string& label, int to) { add(label + " ...", [this, to, root] { openMenu(to, root); }); };

    switch (page) {
        case MenuSources: {
            auto source = [&](const std::string& label, bool Settings::*flag) {
                addSetting(label, [this, flag]() -> std::string { return _settings.*flag ? "Shown" : "不显示"; }, [this, flag](int) { toggleSource(_settings.*flag); });
            };
            source("SD 卡", &Settings::fromSd);
            source("USB 驱动器", &Settings::fromUsb);
            source("网络上的电脑", &Settings::fromPcNetwork);
            source("USB 线连接的电脑", &Settings::fromPcCable);
            if (_console)
                source("游戏卡带", &Settings::fromGameCard);
            // Only worth asking while both are looked in: a PC on both is then only listed once, the way this says.
            if (_settings.fromPcNetwork && _settings.fromPcCable)
                addSetting("PC 同时连接两端", [this]() -> std::string { return _settings.wifiFirst ? "优先 Wi-Fi" : "优先 USB 数据线"; }, [this](int) { toggleWifiFirst(); });
            add(_settings.filesFolder.empty() ? "仅搜索一个文件夹" : "仅搜索文件夹： " + _settings.filesFolder, [this] { chooseFilesFolder(); });
            // Looking everywhere leaves some folders out, to be quick and easy on the SD card, and a game kept in one would otherwise just
            // never show up, with nothing to say why.
            if (_settings.filesFolder.empty()) {
                add("排除范围", [this] {
                    _menu.reset();
                    showMessage("排除范围", "搜索所有位置时会跳过系统与自制固件的文件夹（Nintendo、emuMMC、"
                                                   "atmosphere、bootloader）、config、SaltySD 以及隐藏文件夹，这些地方不会放游戏。switch 目录下会查看每个"
                                                   "应用的文件夹及其直属子文件夹，但不会深入应用自己的目录。\n\n"
                                                   "若要查看其中之一，请用「仅查看一个文件夹」指定：所选文件夹会"
                                                   "逐层向下完整查看。");
                });
            }
            break;
        }
        case MenuThisGame:
            // Details, whatever the game: its own page where it can be opened, and what's noted about it where it can't (an extracted game,
            // which is a folder already, and an update file over an installed game), so "its details" always means the same place.
            if (facts.canLook) {
                add("详细信息", [this] { openInside(true); });
                add("查看内部", [this] { openInside(false); });
            } else if (!facts.about.empty()) {
                add("详细信息", [this, about = facts.about, name = facts.aboutName] {
                    _menu.reset();
                    showMessage(name, joined(about, "\n\n"));
                });
            }
            if (_tab != TabInstalled && _console)
                add("安装此游戏", [this] { onlyTheSelected([this] { pressInstall(); }); });
            if (facts.anyFile) {
                add("解包此游戏", [this] { onlyTheSelected([this] { doWhenFree([this] { beginExtract(); }); }); });
                add("检查此游戏是否损坏", [this] { onlyTheSelected([this] { doWhenFree([this] { beginCheck(); }); }); });
            }
            add("重新分析此游戏", [this] { onlyTheSelected([this] { analyzeAgain(); }); });
            add("从列表隐藏此游戏", [this] { hideFocused(); });
            if (!facts.removeThis.empty())
                add(facts.removeThis, [this] { askToRemove(false); });
            break;
        case MenuTicked:
            if (facts.anyFile) {
                add("解包已勾选游戏", [this] { doWhenFree([this] { beginExtract(); }); });
                add("检查已勾选游戏是否损坏", [this] { doWhenFree([this] { beginCheck(); }); });
            }
            add("重新分析已勾选游戏", [this] { analyzeAgain(); });
            if (!facts.removeTicked.empty())
                add(format("%s (%d)", facts.removeTicked.c_str(), facts.ticked), [this] { askToRemove(true); });
            break;
        case MenuInside: {
            // What Details and Look inside can do, as their buttons say, and what there's no room for along the bottom.
            bool opened = false, asBytes = false, deeper = false, saving = false;
            std::string action;
            {
                sys::Lock lock(_mutex);
                if (_inside) {
                    opened = _inside->ready && _inside->problem.empty();
                    asBytes = _inside->asBytes;
                    deeper = _inside->folder != 0 || _inside->foundFrom >= 0;
                    saving = _inside->saving;
                }
                action = detailsActionLocked();
            }
            if (_screen == Screen::Details) {
                if (opened)
                    add("查看内部", [this] { goInside(); });
                if (!action.empty()) {
                    add(action + "此游戏", [this] {
                        closeInside();
                        onlyTheSelected([this] { pressCompress(); });
                    });
                }
                // Extracting and checking it, as This game has them, so a game's own page has all that can be done to it, as in every version.
                if (facts.anyFile) {
                    add("解包此游戏", [this] {
                        closeInside();
                        onlyTheSelected([this] { doWhenFree([this] { beginExtract(); }); });
                    });
                    add("检查此游戏是否损坏", [this] {
                        closeInside();
                        onlyTheSelected([this] { doWhenFree([this] { beginCheck(); }); });
                    });
                }
            } else if (opened && !saving) {
                add("保存到 SD 卡", [this] {
                    sys::Lock lock(_mutex);
                    saveInsideFocusedLocked();
                });
                add("按名称查找文件", [this] { findInside(); });
                add(asBytes ? "按类型显示文件" : "以字节显示文件", [this] {
                    sys::Lock lock(_mutex);
                    toggleInsideBytesLocked();
                });
                if (deeper) {
                    add("回到顶部", [this] {
                        sys::Lock lock(_mutex);
                        insideToTopLocked();
                    });
                }
                add("详细信息", [this] {
                    sys::Lock lock(_mutex);
                    if (_inside) {
                        _inside->fromDetails = true;
                        _screen = Screen::Details;
                        _dirty = true;
                    }
                });
            }
            addPage("帮助", MenuHelp);
            break;
        }
        case MenuResults:
            // What's worth doing about what just happened: telling about it when something went wrong, and the help.
            add("报告问题", [this] { reportProblem(); });
            addPage("帮助", MenuHelp);
            break;
        case MenuHelp:
            add("报告问题", [this] { reportProblem(); });
            // What there is to know about this app, and getting its newest version, are here, where people look for them.
            add("检查新版本", [this] {
                _menu.reset();
                startUpdateCheck(true);
            });
            add(format("关于 hacBrewPackLZ %s", AppVersion), [this] {
                _menu.reset();
                showAbout();
            });
            for (size_t i = 0; i < HelpTopicCount; i++)
                add(HelpTopics[i].title, [this, i] { showHelpTopic(i); });
            break;
        case MenuList: {
            add("刷新列表", [this] { refreshList(); });
            if (facts.rows > 0 && facts.ticked == 0)
                add("重新分析所有游戏", [this] { analyzeAgain(); });
            if (facts.hidden > 0)
                add(format("重新显示隐藏的游戏（%d）", (int)facts.hidden), [this] { showHidden(); });
            break;
        }
        default:
            // The overview isn't a list, so all there is to choose from it is what applies everywhere: where games are looked for, looking
            // again for what's been plugged in or shared since, the measuring that goes on by itself, help and the settings.
            if (_overview) {
                addPage("搜索位置", MenuSources);
                add("重新查找", [this] {
                    _menu.reset();
                    lookAgain();
                });
                bool measuring = _analystRunning && _analystMeasuring;
                bool stopped = _settings.thorough && _settings.compress && !_autoAnalyze[TabInstalled] && !_autoAnalyze[TabFiles];
                if (measuring) {
                    add("停止测量", [this] {
                        _menu.reset();
                        stopMeasuring();
                    });
                } else if (stopped) {
                    add("继续测量", [this] {
                        _menu.reset();
                        goOnMeasuring();
                    });
                }
                addPage("帮助", MenuHelp);
                add("设置 …", [this] { openSettings(); });
                break;
            }
            addSetting("排序方式", [this]() -> std::string {
                static const char* const names[] = {"节省最多", "大小", "名称"};
                return names[_settings.sort];
            }, [this](int direction) { changeSort(direction); });
            if (facts.ticked > 0 && _tab != TabInstalled && _console)
                add("安装已勾选游戏", [this] { pressInstall(); });
            if (_analystRunning && _analyzingTab == _tab)
                add(_analystMeasuring ? "停止测量" : "停止分析", [this] { stopAnalyzing(); });
            else if (!_autoAnalyze[_tab] && facts.toAnalyze > 0)
                add(format("继续分析（剩余 %d）", (int)facts.toAnalyze), [this] { _autoAnalyze[_tab] = true; });
            if (_tab != TabInstalled)
                addPage("搜索位置", MenuSources);
            if (facts.rows > 0)
                addPage("此游戏", MenuThisGame);
            if (facts.ticked > 0)
                addPage(format("已勾选游戏（%d）", facts.ticked), MenuTicked);
            addPage("列表", MenuList);
            addPage("帮助", MenuHelp);
            add("设置 …", [this] { openSettings(); });
            break;
    }
    menu->page = page;
    menu->root = root;
    // It opens where it is, rather than coming from wherever it was last time.
    _glideList = -1;
    _menu = std::move(menu);
    _dirty = true;
}

// -------------------------------------------------------------------------------------------------------------------
// Choosing a folder

void App::browse(const std::string& title, const std::string& start, const std::string& useDefaultLabel, std::function<void(const std::string&)> choose) {
    _browser.reset(new Browser);
    _glideList = -1;
    _browser->title = title;
    _browser->useDefaultLabel = useDefaultLabel;
    _browser->choose = std::move(choose);
    _browseFrom = _screen;
    _screen = Screen::Browse;
    _drives = _platform.drives();
    browseTo(!start.empty() && sys::isDirectory(start) ? start : std::string());
}

void App::browseTo(std::string path) {
    Browser& browser = *_browser;
    browser.path = path;
    browser.entries.clear();
    browser.problem.clear();
    browser.focus = 0;
    browser.scroll = 0;
    _dirty = true;

    if (path.empty()) {
        for (auto& drive : _platform.drives())
            browser.entries.push_back(Browser::Entry{drive.name, drive.root, ""});
        // A PC running hacbrewpacklz send, over the network or a cable.
        for (auto& peer : remote::peers())
            browser.entries.push_back(Browser::Entry{peer.name, "pc:" + peer.id + "/", ""});
        return;
    }

    std::vector<std::string> names;
    try {
        for (auto& entry : sys::list(path)) {
            if (entry.isDirectory)
                names.push_back(entry.name);
        }
    } catch (const ToolError& e) {
        // Most likely a PC that isn't there, or isn't answering.
        browser.problem = e.what();
        logLine(path + ": " + e.what());
        return;
    }
    std::sort(names.begin(), names.end(), lessNoCase);
    auto skip = _platform.foldersToSkip();
    // Saying which are games means looking inside each, which is only worth it for a folder of a sensible size.
    constexpr size_t MostToLookInto = 256;
    for (auto& name : names) {
        std::string full = joinPath(path, name);
        if (std::any_of(skip.begin(), skip.end(), [&](const std::string& s) { return equalsNoCase(full, s); }))
            continue;
        std::string note;
        try {
            note = names.size() <= MostToLookInto && ExtractedGame::isOne(full) ? "已解包的游戏" : "";
        } catch (const ToolError&) {
        }
        browser.entries.push_back(Browser::Entry{name, full, note});
    }
}

void App::browseInto(int index) {
    if (index >= 0 && index < (int)_browser->entries.size())
        browseTo(_browser->entries[(size_t)index].path);
}

void App::closeBrowser() {
    _browser.reset();
    _screen = _browseFrom;
    _dirty = true;
}

void App::browseUp() {
    std::string from = _browser->path;
    if (from.empty()) {
        closeBrowser();
        return;
    }
    bool isRoot = false;
    for (auto& drive : _platform.drives())
        isRoot = isRoot || equalsNoCase(drive.root, from);
    // A PC's own folder, pc:<name>/, is as far up as it goes there.
    isRoot = isRoot || (remote::isRemote(from) && from.find('/') == from.size() - 1);
    std::string to;
    if (!isRoot) {
        size_t slash = from.find_last_of('/');
        if (slash != std::string::npos) {
            to = from.substr(0, slash);
            // Up to a root keeps the slash that makes it one.
            if (!to.empty() && (to.back() == ':' || (remote::isRemote(to) && to.find('/') == std::string::npos)))
                to += '/';
        }
        for (auto& drive : _platform.drives()) {
            if (equalsNoCase(drive.root, to + "/"))
                to = drive.root;
        }
    }
    browseTo(to);
    for (size_t i = 0; i < _browser->entries.size(); i++) {
        if (equalsNoCase(_browser->entries[i].path, from))
            _browser->focus = (int)i;
    }
}

/// The folder being looked at, or with an empty path, whatever's usual. By value, as it's usually the browser's own, which goes first.
void App::browseChoose(std::string path) {
    auto choose = _browser->choose;
    closeBrowser();
    choose(path);
}

void App::browseUse() {
    if (!_browser->path.empty())
        browseChoose(_browser->path);
}

void App::browseUseDefault() {
    if (!_browser->useDefaultLabel.empty())
        browseChoose("");
}

// -------------------------------------------------------------------------------------------------------------------
// Settings, and the welcome screen

std::string App::shownFolder(const std::string& path, const char* otherwise) {
    if (path.empty())
        return otherwise;
    for (auto& drive : _platform.drives()) {
        if (startsWithNoCase(path, drive.root)) {
            std::string rest = path.substr(drive.root.size());
            return drive.name + ": /" + (rest.empty() || rest[0] != '/' ? rest : rest.substr(1));
        }
    }
    return path;
}

static std::function<std::string()> onOff(const bool& setting) {
    return [&setting] { return setting ? std::string("开") : std::string("关"); };
}

App::OptionItem App::installedGamesOption(const char* section) {
    return OptionItem{section, "已安装游戏", "选择「替换」会在新副本写好后释放空间；选择「保存为 NSP」则保留原游戏，不释放空间。",
                      [this] { return replacesInstalled() ? std::string("替换它们") : std::string("保存为 NSP"); },
                      [this](int) {
                          _settings.replaceInstalled = !_settings.replaceInstalled;
                          saveSettings();
                      }};
}

App::OptionItem App::gameFilesOption(const char* section, const char* help) {
    return OptionItem{section, "游戏文件和文件夹", help,
                      [this] { return _settings.installFiles ? std::string("安装它们") : std::string("保存为 NSP"); },
                      [this](int) {
                          _settings.installFiles = !_settings.installFiles;
                          saveSettings();
                          // What's ticked, and what's done, depends on whether game files get installed, and so does which copy of a game is
                          // used, as only saving NSPs puts a game's DLC in its file.
                          sys::Lock lock(_mutex);
                          if (_scanned[TabFiles])
                              showIndexRowsLocked(_listedPaths, true);
                          judgeAllLocked();
                      }};
}

App::OptionItem App::nspFolderOption(const char* section) {
    return OptionItem{section, "NSP 输出位置", "按 A 键选择 SD 卡或 USB 盘上的文件夹。",
                      [this] { return shownFolder(nspFolder(), ""); },
                      [this](int) {
                          browse("NSP 输出位置", nspFolder(), "使用默认文件夹", [this](const std::string& path) {
                              _settings.nspFolder = path;
                              saveSettings();
                          });
                      },
                      true};
}

App::OptionItem App::minimumSavingOption(const char* section) {
    return OptionItem{section, "最小节省比例", "实测节省比例低于此值的游戏会归入「已完成」，把时间用在收益最大的游戏上。仍可手动勾选它们。",
                      [this] { return _settings.minSaving == 0 ? std::string("关") : format("%d%%", _settings.minSaving); },
                      [this](int direction) {
                          _settings.minSaving = nextChoice(MinSavingChoices, _settings.minSaving, direction);
                          saveSettings();
                          sys::Lock lock(_mutex);
                          judgeAllLocked();
                      },
                      false, true};
}

App::OptionItem App::autoUpdateOption(const char* section) {
    return OptionItem{section, "保持程序为最新版本", "每次启动时联网检查并安装新版本，下载内容必须通过签名校验。", onOff(_settings.autoUpdate),
                      [this](int) {
                          _settings.autoUpdate = !_settings.autoUpdate;
                          saveSettings();
                      }};
}

/// What's being worked out at the moment is for the settings as they were, and everything that was worked out for them has to be again.
void App::compressionChanged() {
    quietenWorker();
    saveSettings();
    sys::Lock lock(_mutex);
    for (auto& row : _rows) {
        row.hasEstimate = false;
        row.belowMinimum.clear();
        // Done was for the settings as they were, as on the PC.
        if (row.state == RowState::Failed || row.state == RowState::Done) {
            row.state = RowState::Ready;
            row.message.clear();
        }
        recallEstimateLocked(row);
    }
    judgeAllLocked();
    // What's hidden as done, or comes back, depends on it, and the list shows what's on it in order.
    sortRowsLocked();
    _dirty = true;
    _autoAnalyze[TabInstalled] = _autoAnalyze[TabFiles] = true;
}

void App::buildOptions() {
    _options.clear();
    auto add = [this](const char* section, const char* label, const char* help, std::function<std::string()> value, std::function<void(int)> change, bool action = false) {
        _options.push_back(OptionItem{section, label, help, std::move(value), std::move(change), action});
    };
    auto toggle = [this](bool& setting) {
        return [this, &setting](int) {
            setting = !setting;
            saveSettings();
        };
    };

    const char* section = "值得处理的游戏";
    _options.push_back(minimumSavingOption(section));
    add(section, "分析方式", "「完整」会在快速初览后，于程序运行期间逐个通读游戏（优先处理可能改变结论的游戏），得出精确数值并勾选值得处理的游戏；会为你启动的任务让路，且只写入分析结果。「快速」只看初览，无法确认的游戏保持未勾选。",
        [this] { return _settings.thorough ? std::string("完整") : std::string("快速"); }, [this](int) {
            _settings.thorough = !_settings.thorough;
            saveSettings();
            // Back to quick, measuring that was going on by itself stops, even beside a list being looked through, which picks up again, and
            // even while it waits for a first look in the middle of it.
            if (!_settings.thorough && _analystRunning && _analystStartedMeasuring)
                stopAnalyst();
            sys::Lock lock(_mutex);
            _dirty = true;
        });
    add(section, "隐藏 1 MB 以下的游戏", "跳过无更新补丁或 DLC 的超小游戏，例如主菜单上的自制程序快捷方式。", onOff(_settings.hideSmall), [this](int) {
        _settings.hideSmall = !_settings.hideSmall;
        saveSettings();
        sys::Lock lock(_mutex);
        _dirty = true;
    });

    section = "压缩";
    add(section, "压缩", "关闭时仅按原样重打包，并把更新补丁并入本体，速度更快；只有含补丁的游戏才会变小。", onOff(_settings.compress), [this](int) {
        _settings.compress = !_settings.compress;
        compressionChanged();
        // What only goes with compressing comes and goes with it.
        _rebuildOptions = true;
    });
    add(section, "压缩级别", "1 和 2 为 LZ4，速度最快；3～12 为 LZ4 HC，体积更小但更慢，9 较为均衡。每个级别都需"
        "单独分析。", [this] { return format("%d", _settings.level); }, [this](int direction) {
        _settings.level = std::max(1, std::min(12, _settings.level + direction));
        compressionChanged();
    });
    _options.back().repeats = true;

    section = "处理方式";
    _options.push_back(installedGamesOption(section));
    _options.push_back(gameFilesOption(section, "「保存为 NSP」会把成品放进 NSP 文件夹；「安装」按「安装游戏到」的设置直接装好可玩。+ 菜单也可安装已勾选的游戏，不受此设置限制。"));
    add(section, "游戏与 DLC 打包在同一文件", "保存 NSP 时：开启后，按游戏 ID 全量收集本体、更新与 DLC 一并写入单一文件，每个标题各保留一份内容元数据，不受文件排列顺序影响；没有本体的 DLC 也会合并到一个文件里。关闭时，每个 DLC 单独生成一个文件。已安装游戏的 DLC 仍与主机一致分别存放。",
        onOff(_settings.dlcInGameFile), [this](int) {
            _settings.dlcInGameFile = !_settings.dlcInGameFile;
            saveSettings();
            // Which copy of a game is used, and what's left to do with it, depend on it.
            sys::Lock lock(_mutex);
            if (_scanned[TabFiles])
                showIndexRowsLocked(_listedPaths, true);
        });
    add(section, "安装游戏到", "从文件或文件夹添加的游戏存放位置；已安装的游戏被替换时仍在原位置。",
        [this] { return _settings.installToConsole ? std::string("主机内存") : std::string("SD 卡"); }, toggle(_settings.installToConsole));
    add(section, "文件安装完成后", "安装完成后删除原文件可释放其占用空间，且仅在安装并校验通过后执行；其他情况下不会动任何文件。",
        [this] { return _settings.deleteInstalled ? std::string("删除文件") : std::string("保留文件"); }, toggle(_settings.deleteInstalled));
    add(section, "提升处理器频率", "耗时更短但工作时更耗电，与主机加载游戏时的状态类似。", onOff(_settings.boost), toggle(_settings.boost));

    section = "文件夹";
    _options.push_back(nspFolderOption(section));
    add(section, "解包输出位置", "按 A 键选择 SD 卡或 USB 盘上的文件夹，其中的游戏文件会出现在列表中。", [this] { return shownFolder(extractFolder(), ""); }, [this](int) {
        browse("解包输出位置", extractFolder(), "使用默认文件夹", [this](const std::string& path) {
            quietenWorker();
            _settings.extractFolder = path;
            saveSettings();
            forgetFileScan();
        });
    }, true);

    section = "本程序";
    _options.push_back(autoUpdateOption(section));
    _options.push_back(sendProblemsOption(section));
    add(section, "记录日志", "把程序运行过程写入 SD 卡上的文件，出问题时可查看或发送；会略微增加写卡量。", onOff(_settings.writeLog), [this](int) {
        _settings.writeLog = !_settings.writeLog;
        saveSettings();
        {
            sys::Lock lock(_logMutex);
            _log.reset();
            _logPath.clear();
            _logUnwritten.clear();
        }
        if (_settings.writeLog) {
            _logPosition = 0;
            openLog();
        }
    });
    add(section, "检查新版本", "在 GitLab 上检查新版本，如有则安装。", [this] {
        sys::Lock lock(_mutex);
        return _updateState;
    }, [this](int) { startUpdateCheck(true); }, true);
    add(section, "欢迎界面", "立即检查新版本，如有则安装。", [] { return std::string(); }, [this](int) {
        _welcomeFocus = 0;
        _welcomeFromSettings = true;
        _screen = Screen::Welcome;
        _dirty = true;
    }, true);
    add(section, "关于", "版本号、密钥来源和日志位置。", [] { return std::string(); }, [this](int) { showAbout(); }, true);

    // What most people will never need to change is out of the way until it's asked for.
    static const char* const Rarely[] = {"记录日志", "隐藏 1 MB 以下的游戏", "压缩", "压缩级别", "游戏与 DLC 打包在同一文件", "文件安装完成后", "提升处理器频率", "保持程序为最新版本", "发送问题报告", "检查新版本", "欢迎界面", "关于"};
    if (!_settings.moreSettings) {
        _options.erase(std::remove_if(_options.begin(), _options.end(), [](const OptionItem& item) {
            return std::any_of(std::begin(Rarely), std::end(Rarely), [&](const char* label) { return item.label == label; });
        }), _options.end());
    }
    // With compression off, nothing gets compressed or saves anything to go by, so these have nothing to say.
    static const char* const OnlyWhenCompressing[] = {"最小节省比例", "分析方式", "压缩级别"};
    if (!_settings.compress) {
        _options.erase(std::remove_if(_options.begin(), _options.end(), [](const OptionItem& item) {
            return std::any_of(std::begin(OnlyWhenCompressing), std::end(OnlyWhenCompressing), [&](const char* label) { return item.label == label; });
        }), _options.end());
    }
    _options.push_back(OptionItem{"", _settings.moreSettings ? "收起高级设置" : "更多设置", "压缩级别、程序更新等很少需要改动的设置。", [] { return std::string(); }, [this](int) {
        _settings.moreSettings = !_settings.moreSettings;
        saveSettings();
        _rebuildOptions = true;
    }, true});
}

/// What most people will want a say in, all in one place, the first time this opens. All of it can be changed later in Settings, from the + menu.
void App::buildWelcome() {
    _welcome.clear();
    const char* section = "压缩时";
    _welcome.push_back(installedGamesOption(section));
    _welcome.push_back(gameFilesOption(section, "「安装」会在安装过程中压缩并装到主机，装好即可游玩；「保存为 NSP」会把成品放进 NSP 文件夹。"));
    _welcome.push_back(nspFolderOption(section));
    _welcome.push_back(minimumSavingOption("值得处理的游戏"));
    _welcome.push_back(autoUpdateOption("本程序"));
    _welcome.push_back(sendProblemsOption("本程序"));
    _welcome.push_back(OptionItem{"", "开始", "以上设置之后都可在 + 菜单的「设置」中修改。", [] { return std::string(); }, [this](int) { finishWelcome(); }, true});
}

void App::finishWelcome() {
    _settings.welcomed = true;
    // Sending problems is on the welcome, so that's asked too.
    _settings.problemsAsked = true;
    saveSettings();
    _screen = _welcomeFromSettings ? Screen::Settings : Screen::Main;
    _welcomeFromSettings = false;
    _dirty = true;
}

void App::openSettings() {
    _menu.reset();
    _screen = Screen::Settings;
    _dirty = true;
}

void App::closeSettings() {
    _screen = Screen::Main;
    _dirty = true;
}

}  // namespace hbp
