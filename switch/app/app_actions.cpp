// What people can do: ticking games, the + menu and the questions it asks, choosing a folder, and the settings.
#include "app_internal.h"

#include "../core/remote.h"

namespace hbp {

using namespace app;

void App::showMessage(const std::string& title, const std::string& text) {
    auto dialog = std::make_unique<Dialog>();
    dialog->title = title;
    dialog->paragraphs = {text};
    dialog->choices = {{"OK", nullptr}};
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
    dialog->title = format("hacBrewPackLZ %s", AppVersion);
    dialog->paragraphs.push_back("以固件本身可读取的形式压缩存储游戏，占用空间更小，运行效果相同。");
    dialog->paragraphs.push_back("密钥：" + (_keysPath.empty() ? std::string("未找到") : _keysPath));
    if (_log)
        dialog->paragraphs.push_back("运行记录写入" + _logPath);
    dialog->paragraphs.push_back("Free software under the GNU General Public License, version 2. USB drives are read with libusbhsfs, NTFS-3G and lwext4, under the same licence.");
    dialog->choices = {{"OK", nullptr}};
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
        bool wanted = action == TickAction::All || (action == TickAction::WorthIt && row->belowMinimum.empty() && row->state != RowState::Done);
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
            return "Tick all";
        case TickAction::None:
            return "Untick all";
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
            showToast("Everything ticked");
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
    if (tab == _tab || tab < 0 || tab >= TabCount)
        return;
    _tab = tab;
    _focusShown = _focus[tab];
    _scrollShown = _scroll[tab];
    _dirty = true;
    // A list that hasn't come up yet goes first, as it's what's being looked at. What it took the place of picks up again by itself afterwards.
    if (tab != TabSettings && !_scanned[tab] && _jobKind == JobKind::Scanning && _scanningTab != tab)
        _scanStop = true;
}

std::string App::sortDescription() {
    switch (_settings.sort) {
        case 0:
            return _sortedByGain[_tab] ? "best saving first" : "分析完成前按名称排序";
        case 1:
            return "largest first";
        default:
            return "by name";
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

void App::setHideDone(bool hide) {
    _settings.hideCompressed = hide;
    saveSettings();
    sys::Lock lock(_mutex);
    _dirty = true;
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

/// Forgets what was worked out for what's ticked, or for everything if nothing is, so that it gets worked out again.
void App::analyzeAgain() {
    quietenWorker();
    int count = 0;
    {
        sys::Lock lock(_mutex);
        auto rows = visibleRowsLocked(_tab);
        bool anyTicked = std::any_of(rows.begin(), rows.end(), [](const Row* row) { return row->ticked && row->blocked.empty(); });
        for (auto* row : rows) {
            if (!row->blocked.empty() || (anyTicked && !row->ticked))
                continue;
            _cache.erase(cacheKey(*row));
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
        _sortedByGain[_tab] = false;
        sortRowsLocked();
        _dirty = true;
    }
    _autoAnalyze[_tab] = true;
    showToast(count == 1 ? "Analyzing it again" : format("Analyzing %d games again", count));
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
    if (_analystRunning)
        stopAnalyzing();
    else if (toAnalyze || !_scanned[_tab])
        _autoAnalyze[_tab] = false;
    if (!busy() && ready) {
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
        updating = _updating && _updateState.rfind("正在获取", 0) == 0;
    }
    bool analyzing = _analystRunning;
    auto dialog = std::make_unique<Dialog>();
    dialog->title = "Quit hacBrewPackLZ?";
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
    std::vector<std::string> names;
    {
        sys::Lock lock(_mutex);
        for (auto* row : visibleRowsLocked(_tab)) {
            bool beingInstalled = row->tab != TabInstalled && goesOnConsole(*row);
            if (row->ticked && row->blocked.empty() && !row->hasUpdate && row->extracted.empty() && !beingInstalled)
                names.push_back(row->name);
        }
    }
    if (names.empty())
        return false;

    auto dialog = std::make_unique<Dialog>();
    bool one = names.size() == 1;
    dialog->title = one ? "此游戏没有升级补丁" : format("%d games have no update", (int)names.size());
    dialog->paragraphs.push_back(std::string("关闭压缩时，重打包只能通过把升级补丁并入本体来缩小体积。") + (one ? "这个处理后体积不变，白费时间，可以取消勾选。" : "这些处理后体积不变，白费时间，可以取消勾选。"));
    dialog->paragraphs.push_back(someNames(names, 3) + ".");
    dialog->choices.push_back({one ? "取消勾选它" : "取消勾选它们", [this] {
        bool anyLeft = false;
        {
            sys::Lock lock(_mutex);
            for (auto* row : visibleRowsLocked(_tab)) {
                if (row->ticked && row->blocked.empty() && !row->hasUpdate && row->extracted.empty()) {
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
            showMessage("没有可重打包的游戏", "已勾选的游戏中，没有一个带有可并入的升级补丁。");
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

void App::askToCompress() {
    if (_workerRunning)
        return;
    if (askAboutGamesWithoutUpdates())
        return;

    int count = 0, unknown = 0, extracted = 0, onConsole = 0, toFile = 0;
    i64 saved = 0;
    {
        sys::Lock lock(_mutex);
        for (auto* row : visibleRowsLocked(_tab)) {
            if (!row->ticked || !row->blocked.empty())
                continue;
            count++;
            extracted += row->extracted.empty() ? 0 : 1;
            if (goesOnConsole(*row))
                onConsole++;
            else
                toFile++;
            if (row->hasEstimate)
                saved += row->estimate.savedBytes();
            else
                unknown++;
        }
    }
    bool replacing = _tab == TabInstalled && replacesInstalled();
    bool installing = _tab != TabInstalled && onConsole > 0;
    std::string word = installing ? "安装" : verb();
    std::string lower = word;
    lower[0] = (char)tolower((u8)lower[0]);
    if (count == 0) {
        showMessage("未勾选任何游戏", "在要处理的游戏上按 A 键以" + lower + "，然后重试。");
        return;
    }

    if (onConsole > 0 && _settings.compress && !_platform.firmwareReadsCompression()) {
        showMessage("此固件版本过旧", "Games stored this way need firmware 12.0.0 or newer. Saving an NSP still works: you can choose that in the settings.");
        return;
    }

    // Better said now than found out part of the way through.
    std::string folder = nspFolder();
    if (toFile > 0) {
        sys::makeDirectories(folder);
        if (!sys::isDirectory(folder)) {
            showMessage("无法访问该文件夹", "NSP 设置为存放到" + folder + "，但该位置不存在。如果在 USB 盘上，请插好；也可以在设置里另选文件夹。");
            return;
        }
    }
    RoomPlan plan = planRoom();
    std::string notFitting = someNames(plan.wontFit, 3);
    bool tooLarge = plan.reason.find("can't hold") != std::string::npos;
    if (plan.order.empty()) {
        if (tooLarge)
            showMessage("空间不足，无法容纳", notFitting + ": " + plan.reason + "。FAT32 单文件上限为 4 GB，需要改用 exFAT 或 NTFS。");
        else
            showMessage("There isn't enough room", format("%s: %s more needs to be free on %s.", notFitting.c_str(), formatSize(plan.mostNeeded).c_str(), plan.reason.c_str()));
        return;
    }

    int going = (int)plan.order.size();
    auto dialog = std::make_unique<Dialog>();
    dialog->title = going == 1 ? word + " this game?" : format("%s these %d games?", word.c_str(), going);
    auto say = [&](const std::string& paragraph) { dialog->paragraphs.push_back(paragraph); };
    if (saved > 0 && replacing)
        say(unknown > 0 ? format("This frees about %s, and whatever the ones not analyzed yet gain.", formatSize(saved).c_str()) : format("This frees about %s.", formatSize(saved).c_str()));
    if (replacing) {
        say(std::string(_settings.compress ? "每个游戏都会被自身的压缩副本替换。" : "每个游戏都会被自身的重打包副本替换，并把升级补丁并入其中。") + (_settings.checkResult ? "先写入并校验副本，确认无误后才删除原文件。" : "先写入副本，之后才删除原文件。") + "存档数据不会改动。");
        say("以此方式存储的游戏需要 sigpatches 才能启动，且官方更新无法覆盖安装。");
        if (!_settings.warned)
            say("第一次在这台主机上使用？建议先用一个小游戏试验，确认能启动后再处理其他游戏。");
    } else if (_tab == TabInstalled) {
        say("已安装的游戏保持原样，不会释放空间；每个游戏的 NSP 会保存到" + folder + ".");
    } else if (installing) {
        std::string then = _settings.deleteInstalled ? "安装完成后，该文件会被删除。" : "文件本身不会被修改。";
        say(std::string(_settings.compress ? "每个游戏都会被压缩（已压缩的除外）并安装到 SD 卡" : "每个游戏都按原样安装到 SD 卡") + "，并替换该游戏的旧版本。" + then + "以此方式存储的游戏需要 sigpatches 才能启动。");
    } else if (extracted < count) {
        say(std::string(_settings.compress ? "一个压缩 NSP" : "一个 NSP") + "会保存到" + folder + "。文件本身不会被修改。");
    }
    if (extracted > 0 && !installing)
        say(std::string(extracted == 1 ? "An NSP of the extracted game" : "每个解包后游戏的 NSP") + "会保存到" + folder + "，原文件夹不会被修改。");
    if (!notFitting.empty())
        say(notFitting + (tooLarge ? ": " + plan.reason + "，因此已排除。" : format(": left out, as %s more would have to be free on %s.", formatSize(plan.mostNeeded).c_str(), plan.reason.c_str())));
    if (_memoryLimited)
        say("This was opened from the album, which leaves little memory to work with. For big games, hold R while starting a game to open it instead.");
    int battery = _platform.batteryPercent();
    if (battery >= 0 && battery < 30 && !_platform.isCharging())
        say(format("The battery is at %d%%. It's best to plug the console in: this can take a while.", battery));

    std::vector<std::string> keys = plan.order;
    dialog->choices.push_back({word, [this, keys] {
        if (!_settings.warned) {
            _settings.warned = true;
            saveSettings();
        }
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
    dialog->title = "Stop?";
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
    std::vector<std::string> keys, names;
    bool installed = false, anyFolder = false, anyFile = false;
    i64 bytes = 0;
    {
        sys::Lock lock(_mutex);
        auto rows = visibleRowsLocked(_tab);
        int index = _focus[_tab];
        for (int i = 0; i < (int)rows.size(); i++) {
            const Row& row = *rows[(size_t)i];
            if (allTicked ? !(row.ticked && row.blocked.empty()) : i != index)
                continue;
            keys.push_back(row.key);
            names.push_back(row.name);
            bytes += row.now;
            installed = installed || row.tab == TabInstalled;
            anyFolder = anyFolder || !row.extracted.empty();
            anyFile = anyFile || (row.tab != TabInstalled && row.extracted.empty());
        }
    }
    if (keys.empty())
        return;
    bool one = keys.size() == 1;
    std::string what = installed ? "卸载" : "删除";
    std::string things = installed ? (one ? "game" : "games") : anyFolder && anyFile ? "项目" : anyFolder ? (one ? "文件夹" : "文件夹") : (one ? "文件" : "文件");
    auto dialog = std::make_unique<Dialog>();
    dialog->title = one ? what + " " + names[0] + "?" : format("%s %d %s?", what.c_str(), (int)keys.size(), things.c_str());
    // Most of a list can be ticked without anyone having ticked it, so it says exactly what's about to go.
    if (!one)
        dialog->paragraphs.push_back(someNames(names, 6) + ".");
    dialog->paragraphs.push_back(installed ? format("This takes %s off the console, with updates and DLC. Save data stays.", formatSize(bytes).c_str()) : format("This deletes %s. It can't be undone.", formatSize(bytes).c_str()));
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

// -------------------------------------------------------------------------------------------------------------------
// The + menu

/// What the menu needs to know about the list, to offer only what makes sense for it.
struct App::MenuFacts {
    size_t hidden = 0;
    size_t toAnalyze = 0;
    int rows = 0;
    int ticked = 0;
    int doneHidden = 0;
    int doneShown = 0;
    bool anyFile = false;
    /// What removing the selected game, or the ticked ones, is called here.
    std::string removeThis;
    std::string removeTicked;
    std::string aboutName;
    std::vector<std::string> about;
};

App::MenuFacts App::menuFacts() {
    MenuFacts facts;
    sys::Lock lock(_mutex);
    facts.hidden = _hidden.size();
    facts.toAnalyze = keysToAnalyzeLocked(_tab).size();
    facts.doneHidden = doneHiddenLocked(_tab);
    bool tickedFolder = false, tickedFile = false;
    auto shown = visibleRowsLocked(_tab);
    for (auto* row : shown) {
        facts.rows++;
        bool isTicked = row->ticked && row->blocked.empty();
        facts.ticked += isTicked ? 1 : 0;
        facts.anyFile = facts.anyFile || row->extracted.empty();
        facts.doneShown += isDoneLocked(*row) ? 1 : 0;
        tickedFolder = tickedFolder || (isTicked && !row->extracted.empty());
        tickedFile = tickedFile || (isTicked && row->extracted.empty());
    }
    int index = _focus[_tab];
    if (index >= 0 && index < (int)shown.size()) {
        const Row& focused = *shown[(size_t)index];
        facts.about = focused.notes;
        facts.aboutName = focused.name;
        if (focused.state != RowState::Working)
            facts.removeThis = focused.tab == TabInstalled ? (_console ? "卸载此游戏" : "") : !focused.extracted.empty() ? "删除此文件夹" : "删除此文件";
    }
    if (_tab == TabInstalled)
        facts.removeTicked = _console ? "卸载已勾选的游戏" : "";
    else
        facts.removeTicked = tickedFolder && tickedFile ? "删除已勾选的文件和文件夹" : tickedFolder ? "删除已勾选的文件夹" : "删除已勾选的文件";
    return facts;
}

void App::openMenu(int page) {
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
    auto addPage = [&](const std::string& label, int to) { add(label + " ...", [this, to] { openMenu(to); }); };

    switch (page) {
        case MenuSources: {
            auto source = [&](const std::string& label, bool Settings::*flag) {
                addSetting(label, [this, flag]() -> std::string { return _settings.*flag ? "显示" : "不显示"; }, [this, flag](int) { toggleSource(_settings.*flag); });
            };
            source("SD 卡", &Settings::fromSd);
            source("USB 驱动器", &Settings::fromUsb);
            source("网络上的电脑", &Settings::fromPcNetwork);
            source("A PC on the USB cable", &Settings::fromPcCable);
            add(_settings.filesFolder.empty() ? "仅搜索一个文件夹" : "仅搜索文件夹：" + _settings.filesFolder, [this] { chooseFilesFolder(); });
            break;
        }
        case MenuThisGame:
            if (!facts.about.empty()) {
                add(format("About this game (%d)", (int)facts.about.size()), [this, about = facts.about, name = facts.aboutName] {
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
        case MenuList:
            add("刷新列表", [this] { refreshList(); });
            if (facts.rows > 0 && facts.ticked == 0)
                add("重新分析所有游戏", [this] { analyzeAgain(); });
            if (facts.doneHidden > 0)
                add(format("Show the games that are done (%d)", facts.doneHidden), [this] { setHideDone(false); });
            else if (facts.doneShown > 0)
                add("隐藏已完成的游戏", [this] { setHideDone(true); });
            if (facts.hidden > 0)
                add(format("Show the hidden games again (%d)", (int)facts.hidden), [this] { showHidden(); });
            break;
        default:
            addSetting("排序方式", [this]() -> std::string {
                static const char* const names[] = {"节省最多", "大小", "名称"};
                return names[_settings.sort];
            }, [this](int direction) { changeSort(direction); });
            if (facts.ticked > 0 && _tab != TabInstalled && _console)
                add("安装已勾选游戏", [this] { pressInstall(); });
            if (_analystRunning && _analyzingTab == _tab)
                add("停止分析", [this] { stopAnalyzing(); });
            else if (!_autoAnalyze[_tab] && facts.toAnalyze > 0)
                add(format("Go on analyzing (%d left)", (int)facts.toAnalyze), [this] { _autoAnalyze[_tab] = true; });
            if (_tab != TabInstalled)
                addPage("搜索位置", MenuSources);
            if (facts.rows > 0)
                addPage("此游戏", MenuThisGame);
            if (facts.ticked > 0)
                addPage(format("Ticked games (%d)", facts.ticked), MenuTicked);
            addPage("列表", MenuList);
            break;
    }
    menu->page = page;
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
            note = names.size() <= MostToLookInto && ExtractedGame::isOne(full) ? "Extracted game" : "";
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
    return [&setting] { return setting ? std::string("On") : std::string("Off"); };
}

App::OptionItem App::installedGamesOption(const char* section) {
    return OptionItem{section, "已安装游戏", "Replacing frees the space, once the new copy has been written. Saving an NSP leaves the game as it is and frees nothing.",
                      [this] { return replacesInstalled() ? std::string("替换它们") : std::string("Save an NSP"); },
                      [this](int) {
                          _settings.replaceInstalled = !_settings.replaceInstalled;
                          saveSettings();
                      }};
}

App::OptionItem App::gameFilesOption(const char* section, const char* help) {
    return OptionItem{section, "游戏文件和文件夹", help,
                      [this] { return _settings.installFiles ? std::string("安装它们") : std::string("Save an NSP"); },
                      [this](int) {
                          _settings.installFiles = !_settings.installFiles;
                          saveSettings();
                          // What's ticked, and what's done, depends on whether game files get installed.
                          sys::Lock lock(_mutex);
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
    return OptionItem{section, "最小节省比例", "Games that would save less than this are left unticked, so the time goes where it gains the most. You can still tick them yourself.",
                      [this] { return _settings.minSaving == 0 ? std::string("Off") : format("%d%%", _settings.minSaving); },
                      [this](int direction) {
                          _settings.minSaving = nextChoice(MinSavingChoices, _settings.minSaving, direction);
                          saveSettings();
                          sys::Lock lock(_mutex);
                          judgeAllLocked();
                      },
                      false, true};
}

App::OptionItem App::autoUpdateOption(const char* section) {
    return OptionItem{section, "自动更新", "Each time it opens, it looks for a newer version and installs it, if the console is online. Whatever it gets has to carry the right signature.", onOff(_settings.autoUpdate),
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
        if (row.state == RowState::Failed) {
            row.state = RowState::Ready;
            row.message.clear();
        }
        recallEstimateLocked(row);
    }
    judgeAllLocked();
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
    add(section, "隐藏 1 MB 以下的游戏", "排除没有升级补丁或 DLC 的小体积游戏，转发器就属于这类。", onOff(_settings.hideSmall), [this](int) {
        _settings.hideSmall = !_settings.hideSmall;
        saveSettings();
        sys::Lock lock(_mutex);
        _dirty = true;
    });
    add(section, "隐藏已完成的游戏", "Leaves off what there's nothing left to do with: installed games once they're compressed, and game files once their game is installed compressed, or once they're compressed if they're saved as NSPs. The + menu brings them back.",
        onOff(_settings.hideCompressed), [this](int) { setHideDone(!_settings.hideCompressed); });

    section = "压缩";
    add(section, "压缩", "Off, games are repacked just as they are, with an update made part of the game and no ticket needed. That's quicker, and a game only comes out smaller if it had an update.", onOff(_settings.compress), [this](int) {
        _settings.compress = !_settings.compress;
        compressionChanged();
        // What only goes with compressing comes and goes with it.
        _rebuildOptions = true;
    });
    add(section, "压缩级别", "1 最快，12 压缩率最高，9 比较均衡；每个级别都需要单独分析。", [this] { return format("%d", _settings.level); }, [this](int direction) {
        _settings.level = std::max(1, std::min(12, _settings.level + direction));
        compressionChanged();
    });
    _options.back().repeats = true;
    add(section, "块大小", "块越大压缩率越好，零售游戏使用 64 KB；每种大小都需要单独分析。", [this] { return format("%d KB", _settings.blockSize / 1024); }, [this](int direction) {
        _settings.blockSize = nextChoice(BlockSizeChoices, _settings.blockSize, direction);
        compressionChanged();
    });
    _options.back().repeats = true;

    section = "处理方式";
    _options.push_back(installedGamesOption(section));
    _options.push_back(gameFilesOption(section, "Saving leaves an NSP in the NSP folder. Installing puts the game where Install games to says, ready to play. The + menu can also install what's ticked, whatever this says."));
    add(section, "安装游戏到", "从文件或文件夹添加的游戏存放位置；已安装的游戏被替换时仍在原位置。",
        [this] { return _settings.installToConsole ? std::string("Console") : std::string("SD 卡"); }, toggle(_settings.installToConsole));
    add(section, "文件安装完成后", "Deleting frees the room the file took up, and only happens once the game has been installed and checked. Nothing else ever touches a file.",
        [this] { return _settings.deleteInstalled ? std::string("删除文件") : std::string("保留文件"); }, toggle(_settings.deleteInstalled));
    add(section, "校验写入内容", "Compares what's written with what it should be, and reads some of the game back through the new copy before the original goes. Off is quicker, as whatever fits in memory then doesn't have to be read back at all, but a failing card wouldn't get caught.",
        onOff(_settings.checkResult), toggle(_settings.checkResult));
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
    add(section, "记录日志", "Writes what the app does to a file on the SD card, which is what to look at or send if something goes wrong. It takes a little more writing to the card.", onOff(_settings.writeLog), [this](int) {
        _settings.writeLog = !_settings.writeLog;
        saveSettings();
        {
            sys::Lock lock(_logMutex);
            _log.reset();
        }
        if (_settings.writeLog) {
            _logPosition = 0;
            openLog();
        }
    });
    add(section, "立即检查更新", "在 GitLab 上检查新版本，如有则安装。", [this] {
        sys::Lock lock(_mutex);
        return _updateState;
    }, [this](int) { startUpdateCheck(true); }, true);
    add(section, "欢迎界面", "显示首次打开时的引导内容，把最重要的设置集中在一处。", [] { return std::string(); }, [this](int) {
        _welcomeFocus = 0;
        _screen = Screen::Welcome;
        _dirty = true;
    }, true);
    add(section, "关于", "版本号、密钥来源和日志位置。", [] { return std::string(); }, [this](int) { showAbout(); }, true);

    // What most people will never need to change is out of the way until it's asked for.
    static const char* const Rarely[] = {"记录日志", "隐藏 1 MB 以下的游戏", "压缩", "压缩级别", "块大小", "文件安装完成后", "校验写入内容", "提升处理器频率", "自动更新", "立即检查更新", "欢迎界面", "关于"};
    if (!_settings.moreSettings) {
        _options.erase(std::remove_if(_options.begin(), _options.end(), [](const OptionItem& item) {
            return std::any_of(std::begin(Rarely), std::end(Rarely), [&](const char* label) { return item.label == label; });
        }), _options.end());
    }
    // With compression off, nothing gets compressed or saves anything to go by, so these have nothing to say.
    static const char* const OnlyWhenCompressing[] = {"最小节省比例", "压缩级别", "块大小"};
    if (!_settings.compress) {
        _options.erase(std::remove_if(_options.begin(), _options.end(), [](const OptionItem& item) {
            return std::any_of(std::begin(OnlyWhenCompressing), std::end(OnlyWhenCompressing), [&](const char* label) { return item.label == label; });
        }), _options.end());
    }
    _options.push_back(OptionItem{"", _settings.moreSettings ? "Fewer settings" : "More settings", "压缩级别、写入校验、更新等平时很少需要改动的设置。", [] { return std::string(); }, [this](int) {
        _settings.moreSettings = !_settings.moreSettings;
        saveSettings();
        _rebuildOptions = true;
    }, true});
}

/// What most people will want a say in, all in one place, the first time this opens. All of it can be changed later on the settings tab.
void App::buildWelcome() {
    _welcome.clear();
    const char* section = "压缩时";
    _welcome.push_back(installedGamesOption(section));
    _welcome.push_back(gameFilesOption(section, "Installing puts the game on the console, ready to play, compressed as it goes in. Saving leaves an NSP in the NSP folder."));
    _welcome.push_back(nspFolderOption(section));
    _welcome.push_back(OptionItem{section, "校验写入内容", "Not checking is what most people want. Checking reads every byte back from the card and compares it before the original goes, which catches a failing card and takes a good deal longer.",
                                  [this] { return _settings.checkResult ? std::string("是，更安全") : std::string("否，更快"); },
                                  [this](int) {
                                      _settings.checkResult = !_settings.checkResult;
                                      saveSettings();
                                  }});
    _welcome.push_back(minimumSavingOption("值得处理的游戏"));
    _welcome.push_back(autoUpdateOption("本程序"));
    _welcome.push_back(OptionItem{"", "开始", "这些之后都可以在「设置」页里修改。", [] { return std::string(); }, [this](int) { finishWelcome(); }, true});
}

void App::finishWelcome() {
    _settings.welcomed = true;
    saveSettings();
    _screen = Screen::Main;
    _dirty = true;
}

}  // namespace hbp
