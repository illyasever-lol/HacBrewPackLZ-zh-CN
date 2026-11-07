// The two lists: the games installed on the console, and the games in files and folders wherever they're looked for. How each comes up,
// what goes on it, what's done and what's hidden, and the order it's in.
#include "app_internal.h"

#include "../core/remote.h"

namespace hbp {

using namespace app;

// -------------------------------------------------------------------------------------------------------------------
// Where to look

/// The folder chosen for the list, or else everywhere it's been asked to look: the SD card, USB drives, and PCs sharing their games.
std::vector<std::string> App::fileRoots() {
    if (!_settings.filesFolder.empty())
        return {_settings.filesFolder};
    std::vector<std::string> roots;
    for (auto& drive : _platform.drives()) {
        if (drive.isUsb ? _settings.fromUsb : _settings.fromSd)
            roots.push_back(drive.root);
    }
    std::vector<std::string> pcs = pcRoots();
    roots.insert(roots.end(), pcs.begin(), pcs.end());
    // Where this puts extracted games is always looked in, wherever that is, as long as what it's on is.
    std::string own = extractFolder();
    bool onUsb = false;
    for (auto& drive : _platform.drives())
        onUsb = onUsb || (drive.isUsb && startsWithNoCase(own, drive.root));
    if ((onUsb ? _settings.fromUsb : _settings.fromSd) && !isWithinAny(own, roots))
        roots.push_back(own);
    return roots;
}

/// The PCs sharing their games just now, that the settings say to look in.
std::vector<std::string> App::pcRoots() {
    std::vector<std::string> roots;
    if (_settings.fromPcNetwork || _settings.fromPcCable) {
        for (auto& peer : remote::peers(_settings.fromPcCable)) {
            if (peer.id == "USB" ? _settings.fromPcCable : _settings.fromPcNetwork)
                roots.push_back("pc:" + peer.id + "/");
        }
    }
    std::sort(roots.begin(), roots.end());
    return roots;
}

/// Where a game file is, to say so on the list when it looks in more than one place.
std::string App::whereIs(const std::string& path) {
    if (remote::isRemote(path)) {
        std::string id = path.substr(3, path.find('/') == std::string::npos ? std::string::npos : path.find('/') - 3);
        return id == "USB" ? "在通过数据线连接的 PC 上" : "在" + id;
    }
    for (auto& drive : _drives) {
        if (startsWithNoCase(path, drive.root))
            return drive.isUsb ? "在" + drive.name : "On the SD card";
    }
    return "";
}

/// The list of game files gets looked through again the next time it's due, from the folders up.
void App::forgetFileScan() {
    sys::Lock lock(_mutex);
    _found.walked = false;
    _scanned[TabFiles] = false;
    _openExtracted.reset();
    _openExtractedPath.clear();
}

void App::updateFreeSpace() {
    i64 freeSd = -1, totalSd = 0, freeNand = -1, totalNand = 0, freeUsb = -1, totalUsb = 0;
    if (_console) {
        freeSd = _console->freeBytes(Storage::Sd);
        totalSd = _console->totalBytes(Storage::Sd);
        freeNand = _console->freeBytes(Storage::Nand);
        totalNand = _console->totalBytes(Storage::Nand);
    }
    auto drives = _platform.drives();
    std::string folder = nspFolder();
    const Drive* usb = nullptr;
    for (auto& drive : drives) {
        if (drive.isUsb && (!usb || startsWithNoCase(folder, drive.root)))
            usb = &drive;
    }
    if (usb) {
        freeUsb = sys::freeSpace(usb->root);
        totalUsb = sys::totalSpace(usb->root);
    }
    sys::Lock lock(_mutex);
    _freeSd = freeSd;
    _totalSd = totalSd;
    _freeNand = freeNand;
    _totalNand = totalNand;
    _freeUsb = freeUsb;
    _totalUsb = totalUsb;
    _dirty = true;
}

std::shared_ptr<Image> App::listIconFrom(const Bytes& jpeg) {
    Image decoded;
    if (jpeg.empty() || !_platform.decodeIcon(jpeg, decoded))
        return nullptr;
    return std::make_shared<Image>(decoded.scaledTo(ListIconSize, ListIconSize));
}

/// A row as it now is takes the place of the one before it, keeping what's been done to it. It's locked by whoever calls this.
void App::bringIn(Row row, std::map<std::string, Row>* previous) {
    if (previous) {
        auto before = previous->find(row.key);
        if (before != previous->end()) {
            if (!row.icon)
                row.icon = before->second.icon;
            row.bigIcon = before->second.bigIcon;
            row.ticked = before->second.ticked;
            row.byHand = before->second.byHand;
            row.state = before->second.state;
            row.message = before->second.message;
            row.freed = before->second.freed;
        }
    }
    if (_hidden.count(row.key))
        return;
    if (recallEstimateLocked(row)) {
        judgeLocked(row, convertOptions());
        _sortedByGain[row.tab] = true;
    } else if (!row.blocked.empty() || (row.state == RowState::Failed && !row.byHand)) {
        row.ticked = false;
    }
    _listChanged = _listChanged || row.tab == TabInstalled;
    _rows.push_back(std::move(row));
}

// -------------------------------------------------------------------------------------------------------------------
// Installed games

std::string App::fingerprintOf(const InstalledGame& game) const {
    std::vector<std::string> ids;
    for (auto& meta : game.metas) {
        for (auto& content : meta.contents)
            ids.push_back(content.id.hex());
    }
    std::sort(ids.begin(), ids.end());
    std::string all;
    for (auto& id : ids)
        all += id;
    u8 hash[32];
    sha256(all.data(), all.size(), hash);
    return toHex(hash, 8);
}

App::Row App::rowFor(const InstalledGame& game) {
    Row row;
    row.tab = TabInstalled;
    row.application = game.applicationId;
    row.key = format("%016llx", (unsigned long long)game.applicationId);
    row.name = game.name.empty() ? format("Title %016llX", (unsigned long long)game.applicationId) : game.name;
    row.version = game.displayVersion;
    row.tile = tileColor(row.name);
    applyContents(row, game);
    return row;
}

/// Everything about a row that comes from what's installed, rather than from what the game calls itself.
void App::applyContents(Row& row, const InstalledGame& game) {
    row.now = game.totalBytes();
    row.blocked = game.blocked;
    row.fingerprint = fingerprintOf(game);
    row.ready = true;

    int dlc = 0;
    bool hasBase = false, hasUpdate = false;
    row.gameVersion = 0;
    for (auto& meta : game.metas) {
        if (meta.key.type == Cnmt::MetaApplication) {
            hasBase = true;
            row.storage = meta.storage;
            row.gameVersion = std::max(row.gameVersion, meta.key.version);
        } else if (meta.key.type == Cnmt::MetaPatch) {
            hasUpdate = true;
            row.gameVersion = std::max(row.gameVersion, meta.key.version);
        } else if (meta.key.type == Cnmt::MetaAddOnContent) {
            dlc++;
        }
    }
    row.tiny = hasBase && !hasUpdate && dlc == 0 && row.now < SmallGameBytes;
    row.hasUpdate = hasUpdate;

    std::string version = row.version.empty() ? "" : "版本" + row.version;
    if (hasUpdate)
        version += version.empty() ? "含升级补丁" : "及其升级补丁";
    row.detail = joined({version, dlc == 0 ? "" : format("%d DLC", dlc), row.storage == Storage::Nand ? "在主机上" : row.storage == Storage::Sd ? "On the SD card" : ""}, Dot);

    if (row.blocked.empty() && !hasBase)
        row.blocked = "未安装其本体游戏，因此无法重建";
    if (row.blocked.empty() && row.storage == Storage::GameCard)
        row.blocked = "位于游戏卡带上";
}

void App::scanInstalled() {
    double started = sys::now();
    struct SaveAtTheEnd {
        App& app;
        ~SaveAtTheEnd() { app.saveList(); }
    } save{*this};

    auto list = _console->listApplications();
    updateFreeSpace();
    std::set<std::string> present;
    for (u64 id : list)
        present.insert(format("%016llx", (unsigned long long)id));
    {
        sys::Lock lock(_mutex);
        _jobCount = (int)list.size();
        _jobIndex = 0;
        size_t before = _rows.size();
        _rows.erase(std::remove_if(_rows.begin(), _rows.end(), [&](const Row& r) { return r.tab == TabInstalled && !present.count(r.key); }), _rows.end());
        _listChanged = _listChanged || _rows.size() != before;
    }

    // What's installed only takes a moment to find out, and if a game's contents are what they were, so is what it's called.
    std::vector<InstalledGame> described(list.size());
    std::vector<char> readable(list.size(), 0);
    std::atomic<int> gone{0};
    parallelFor(sys::workerCount(), (int)list.size(), [&](int index, int) {
        checkScanStop();
        size_t i = (size_t)index;
        try {
            described[i] = _console->describe(list[i]);
            readable[i] = 1;
        } catch (const ToolError& e) {
            logLine(format("%016llx can't be read: %s", (unsigned long long)list[i], e.what()));
        }
        sys::Lock lock(_mutex);
        _jobIndex = ++gone;
    });

    std::vector<InstalledGame> toRead;
    for (size_t i = 0; i < list.size(); i++) {
        if (!readable[i])
            continue;
        InstalledGame& game = described[i];
        std::string key = format("%016llx", (unsigned long long)list[i]);
        sys::Lock lock(_mutex);
        Row* known = findLocked(key);
        // What's archived, or on a cartridge that may not even be in there, has nothing here to work with.
        if (game.totalBytes() == 0) {
            if (known) {
                _rows.erase(_rows.begin() + (known - _rows.data()));
                _listChanged = true;
            }
            continue;
        }
        if (_hidden.count(key))
            continue;
        if (known && known->fingerprint == fingerprintOf(game) && !known->name.empty()) {
            std::string before = known->detail;
            applyContents(*known, game);
            judgeLocked(*known, convertOptions());
            _listChanged = _listChanged || known->detail != before;
        } else {
            toRead.push_back(std::move(game));
        }
    }
    logLine(format("%d games installed, %d of them not seen before, gone over in %.1f s", (int)list.size(), (int)toRead.size(), sys::now() - started));

    {
        sys::Lock lock(_mutex);
        _jobCount = (int)toRead.size();
        _jobIndex = 0;
        sortRowsLocked();
        _dirty = true;
    }

    // Each of the rest goes on the list as soon as it's known, with as many being read at a time as there are cores to read them.
    double readingStarted = sys::now();
    std::atomic<int> read{0};
    parallelFor(sys::workerCount(), (int)toRead.size(), [&](int index, int) {
        checkScanStop();
        InstalledGame& game = toRead[(size_t)index];
        try {
            _console->readControl(game);
        } catch (const ToolError& e) {
            logLine(format("The name of %016llx can't be read: %s", (unsigned long long)game.applicationId, e.what()));
        }
        Row row = rowFor(game);
        if (auto icon = listIconFrom(game.icon))
            row.icon = icon;

        int count;
        {
            sys::Lock lock(_mutex);
            std::map<std::string, Row> previous;
            if (Row* known = findLocked(row.key)) {
                previous.emplace(row.key, *known);
                previous[row.key].icon = nullptr;
                _rows.erase(_rows.begin() + (known - _rows.data()));
            }
            bringIn(std::move(row), &previous);
            count = ++read;
            if (count % 16 == 0 || count == (int)toRead.size())
                sortRowsLocked();
            _jobIndex = count;
            _dirty = true;
        }
        // Getting this far shouldn't have to be done again if the app gets closed part way through the list.
        if (count % 64 == 0)
            saveList();
    });
    if (read > 0)
        logLine(format("Read the names and icons of %d games in %.1f s", read.load(), sys::now() - readingStarted));
}

// -------------------------------------------------------------------------------------------------------------------
// Game files: finding them

/// Going through the folders one level at a time, with every level shared out among the threads. It needn't ask about anything but names, except for
/// the size of what turns out to be a game.
App::FoundGames App::lookForGames(const std::vector<std::string>& roots) {
    FoundGames found;
    // Which PCs this looked in, to look again when that changes.
    {
        std::vector<std::string> pcs;
        for (auto& root : roots) {
            if (remote::isRemote(root))
                pcs.push_back(root);
        }
        std::sort(pcs.begin(), pcs.end());
        sys::Lock lock(_pcsMutex);
        _pcsLookedIn = std::move(pcs);
    }
    std::vector<std::string> skip = _platform.foldersToSkip();
    std::vector<std::string> level;
    for (auto& root : roots) {
        if (sys::isDirectory(root) && std::find(level.begin(), level.end(), root) == level.end())
            level.push_back(root);
    }

    int folders = 0;
    std::set<std::string> seen(level.begin(), level.end());
    sys::Mutex collecting;
    while (!level.empty()) {
        logLine(format("Looking through %d folders, starting with %s", (int)level.size(), level[0].c_str()));
        folders += (int)level.size();
        std::vector<std::string> next;
        // Going through folders as fast as it can has been seen to bring down custom firmware's fs.mitm module, and the console with it.
        constexpr int AtOnce = 2;
        parallelFor(AtOnce, (int)level.size(), [&](int index, int) {
            checkScanStop();
            const std::string& folder = level[(size_t)index];
            std::vector<sys::DirEntry> entries = sys::list(folder, false);
            auto has = [&](const char* name) { return std::any_of(entries.begin(), entries.end(), [&](const sys::DirEntry& e) { return e.isDirectory && equalsNoCase(e.name, name); }); };

            // What's in the folder of an extracted game is that game, whatever else it may look like.
            if (has("exefs") && has("control")) {
                i64 npdm = sys::fileSize(folder + "/exefs/main.npdm"), nacp = sys::fileSize(folder + "/control/control.nacp");
                if (npdm >= 0 && nacp >= 0) {
                    i64 romFs = (i64)sys::list(folder + "/romfs", false).size();
                    sys::Lock lock(collecting);
                    found.extracted.emplace_back(folder, npdm + nacp + (romFs << 32));
                }
                return;
            }

            std::vector<std::string> within;
            std::vector<std::pair<std::string, i64>> games;
            for (auto& entry : entries) {
                std::string inside = joinPath(folder, entry.name);
                if (entry.isDirectory) {
                    // A game's own files, even with the rest of it missing, can be thousands of folders of what isn't game files.
                    if (equalsNoCase(entry.name, "romfs") || equalsNoCase(entry.name, "exefs"))
                        continue;
                    if (std::none_of(skip.begin(), skip.end(), [&](const std::string& s) { return equalsNoCase(inside, s); }))
                        within.push_back(std::move(inside));
                } else if (FileArchive::isGameFile(inside)) {
                    games.emplace_back(inside, entry.size >= 0 ? entry.size : sys::fileSize(inside));
                }
            }
            sys::Lock lock(collecting);
            next.insert(next.end(), within.begin(), within.end());
            found.files.insert(found.files.end(), games.begin(), games.end());
        });
        level.clear();
        for (auto& folder : next) {
            if (seen.insert(folder).second)
                level.push_back(folder);
        }
    }

    auto byName = [](const std::pair<std::string, i64>& a, const std::pair<std::string, i64>& b) { return lessNoCase(a.first, b.first); };
    std::sort(found.files.begin(), found.files.end(), byName);
    std::sort(found.extracted.begin(), found.extracted.end(), byName);
    logLine(format("Went through %d folders, and found %d game files and %d extracted games", folders, (int)found.files.size(), (int)found.extracted.size()));
    return found;
}

/// What it takes to put a game file or an extracted game on the list, which is a few small reads and an icon to decode.
App::IndexedFile App::readIndexEntry(const std::string& path, i64 size, bool extracted) {
    IndexedFile entry;
    entry.size = size;
    entry.extracted = extracted;
    // A PC that's sharing a file can say what's in it far faster than reading it from here, over the network, would.
    if (remote::isRemote(path) && fromPc(path, entry))
        return entry;
    logLine("正在打开" + path);
    try {
        if (extracted) {
            ExtractedGame game = openExtracted(path);
            IndexedTitle title;
            title.id = game.titleId;
            title.type = Cnmt::MetaApplication;
            title.version = game.version;
            title.bytes = game.inputBytes;
            title.name = game.name;
            title.icon = listIconFrom(ExtractedGame::readIcon(path));
            entry.titles.push_back(std::move(title));
        } else {
            TitleSet set(_keyset);
            set.add(std::make_shared<FileArchive>(path));
            for (auto& summary : set.summarize()) {
                IndexedTitle title;
                title.id = summary.titleId;
                title.type = summary.type;
                title.version = summary.version;
                title.linked = summary.linkedId;
                title.bytes = summary.bytes;
                title.holdsOnlyPart = summary.holdsOnlyPart;
                title.name = summary.name;
                title.displayVersion = summary.displayVersion;
                title.icon = listIconFrom(summary.icon);
                entry.titles.push_back(std::move(title));
            }
            if (!set.problems.empty())
                entry.problem = set.problems[0].message;
        }
    } catch (const ToolError& e) {
        entry.problem = e.what();
    }
    if (!entry.problem.empty())
        logLine(path + ": " + entry.problem);
    return entry;
}

static std::vector<std::string> splitOn(const std::string& text, char separator) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        size_t end = text.find(separator, start);
        parts.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos)
            return parts;
        start = end + 1;
    }
}

/// What the PC says is in a file it shares. False if it can't say, as an older version can't, and it's to be read from here.
bool App::fromPc(const std::string& path, IndexedFile& entry) {
    Bytes answer;
    try {
        if (!remote::describe(path, answer, [this] { checkScanStop(); }))
            return false;
    } catch (const ToolError&) {
        return false;
    }
    // Lines, a blank line, then the icons one after another.
    std::string text(answer.begin(), answer.end());
    size_t end = text.find("\n\n");
    if (end == std::string::npos)
        return false;
    size_t iconAt = end + 2;
    std::vector<IndexedTitle> titles;
    std::string problem;
    for (auto& line : splitOn(text.substr(0, end), '\n')) {
        auto fields = splitOn(line, '\t');
        if (fields.size() == 2 && fields[0] == "P") {
            problem = fields[1];
        } else if (fields.size() >= 9 && fields[0] == "T") {
            IndexedTitle title;
            title.id = strtoull(fields[1].c_str(), nullptr, 16);
            title.type = (u8)atoi(fields[2].c_str());
            title.version = (u32)strtoul(fields[3].c_str(), nullptr, 10);
            title.linked = strtoull(fields[4].c_str(), nullptr, 16);
            title.bytes = atoll(fields[5].c_str());
            title.holdsOnlyPart = fields[6] == "1";
            size_t iconSize = (size_t)atoll(fields[7].c_str());
            // An earlier PC version leaves out the display version.
            if (fields.size() >= 10) {
                title.displayVersion = fields[8];
                title.name = fields[9];
            } else {
                title.name = fields[8];
            }
            // Against what's left, as a size like -1 would wrap the sum round.
            if (iconSize > answer.size() - iconAt)
                return false;
            if (iconSize > 0)
                title.icon = listIconFrom(Bytes(answer.begin() + (std::ptrdiff_t)iconAt, answer.begin() + (std::ptrdiff_t)(iconAt + iconSize)));
            iconAt += iconSize;
            titles.push_back(std::move(title));
        }
    }
    entry.titles = std::move(titles);
    entry.problem = problem;
    if (!problem.empty())
        logLine(path + ": " + problem);
    return true;
}

/// Going through a whole card can take a while, and there's no need to wait for it to get on with what was there last time, if it still is.
void App::showWhatsStillThere(const std::vector<std::string>& roots) {
    std::vector<std::string> known;
    std::vector<i64> sizes;
    std::vector<char> extracted;
    {
        sys::Lock lock(_mutex);
        known = indexedPathsLocked(roots);
        for (auto& path : known) {
            sizes.push_back(_fileIndex[path].size);
            extracted.push_back(_fileIndex[path].extracted ? 1 : 0);
        }
    }
    std::vector<std::string> stillThere;
    for (size_t i = 0; i < known.size(); i++) {
        checkScanStop();
        if (extracted[i] ? sys::isDirectory(known[i]) : sys::fileSize(known[i]) == sizes[i])
            stillThere.push_back(known[i]);
    }
    sys::Lock lock(_mutex);
    showIndexRowsLocked(stillThere, true);
}

/// The list of game files. Only what's new or has changed gets opened, so after the first time, all it takes is going through the folders.
void App::scanFiles() {
    double started = sys::now();
    {
        sys::Lock lock(_mutex);
        _jobIndex = 0;
        _jobCount = 0;
    }
    updateFreeSpace();
    std::vector<std::string> roots = fileRoots();

    if (!_found.walked) {
        showWhatsStillThere(roots);
        FoundGames fresh = lookForGames(roots);
        fresh.walked = true;
        sys::Lock lock(_mutex);
        _found = std::move(fresh);
    }

    struct ToRead {
        std::string path;
        i64 size;
        bool extracted;
    };
    std::vector<ToRead> toRead;
    std::vector<std::string> paths;
    {
        sys::Lock lock(_mutex);
        std::set<std::string> there;
        auto consider = [&](const std::pair<std::string, i64>& item, bool extracted) {
            there.insert(item.first);
            paths.push_back(item.first);
            auto known = _fileIndex.find(item.first);
            if (known == _fileIndex.end() || known->second.size != item.second || known->second.extracted != extracted)
                toRead.push_back(ToRead{item.first, item.second, extracted});
        };
        for (auto& file : _found.files)
            consider(file, false);
        for (auto& game : _found.extracted)
            consider(game, true);
        // What's gone from where it was isn't anything to know about any more.
        for (auto it = _fileIndex.begin(); it != _fileIndex.end();) {
            if (isWithinAny(it->first, roots) && !there.count(it->first)) {
                it = _fileIndex.erase(it);
                _fileIndexChanged = true;
            } else {
                ++it;
            }
        }
        _jobCount = (int)toRead.size();
    }

    struct SaveAtTheEnd {
        App& app;
        ~SaveAtTheEnd() { app.saveFileIndex(); }
    } save{*this};

    std::atomic<int> read{0};
    parallelFor(sys::workerCount(), (int)toRead.size(), [&](int index, int) {
        checkScanStop();
        const ToRead& item = toRead[(size_t)index];
        IndexedFile entry = readIndexEntry(item.path, item.size, item.extracted);
        int count;
        {
            sys::Lock lock(_mutex);
            _fileIndex[item.path] = std::move(entry);
            _fileIndexChanged = true;
            count = ++read;
            _jobIndex = count;
            if (count % 8 == 0)
                showIndexRowsLocked(paths, false);
            _dirty = true;
        }
        // Getting this far shouldn't have to be done again if the app gets closed part way through the list.
        if (count % 64 == 0)
            saveFileIndex();
    });

    sys::Lock lock(_mutex);
    showIndexRowsLocked(paths, true);
    logLine(format("%d game files and folders on this list, %d of them opened, gone over in %.2f s", (int)paths.size(), (int)toRead.size(), sys::now() - started));
    logWhatsDoneLocked();
}

/// What decides whether a game file is done, for each one whose game is installed.
void App::logWhatsDoneLocked() {
    for (auto& row : _rows) {
        if (row.tab != TabFiles)
            continue;
        for (auto& installed : _rows) {
            if (installed.tab == TabInstalled && installed.application == row.application) {
                logLine(format("%s: the file is v%u (%s), the installed game v%u (%s), %s%s, so the file %s", row.name.c_str(), row.gameVersion, row.version.c_str(), installed.gameVersion, installed.version.c_str(),
                               isCompressed(installed) ? "已压缩" : installed.hasEstimate ? "未压缩" : "尚未分析", installed.ready ? "" : "，尚未查看",
                               isDoneLocked(row) ? "已完成" : "仍待处理"));
                break;
            }
        }
    }
}

// -------------------------------------------------------------------------------------------------------------------
// Game files: what goes on the list

/// Whatever's known of that's under those folders.
std::vector<std::string> App::indexedPathsLocked(const std::vector<std::string>& roots) {
    std::vector<std::string> paths;
    for (auto& item : _fileIndex) {
        if (isWithinAny(item.first, roots))
            paths.push_back(item.first);
    }
    return paths;
}

/// What's known by now goes on the list in place of what was there, keeping what's been done to each.
void App::showIndexRowsLocked(const std::vector<std::string>& paths, bool ready) {
    std::map<std::string, Row> previous;
    for (auto& row : _rows) {
        if (row.tab == TabFiles)
            previous.emplace(row.key, row);
    }
    std::vector<Row> rows = rowsFromIndexLocked(paths);
    _rows.erase(std::remove_if(_rows.begin(), _rows.end(), [&](const Row& r) { return r.tab == TabFiles; }), _rows.end());
    for (auto& row : rows) {
        row.ready = ready;
        bringIn(std::move(row), &previous);
    }
    sortRowsLocked();
    _dirty = true;
}

/// One row for each game, with its update and its DLC, as the installed games have, and of several copies of the same one, the newest. Which
/// update goes with which base game comes out the same as TitleSet::group. Nothing has to be opened to tell.
std::vector<App::Row> App::rowsFromIndexLocked(const std::vector<std::string>& paths) {
    std::vector<IndexedCopy> bases, updates, dlcs;
    std::vector<Row> rows;

    // An extracted game is the game, and what's been made from its folder is done. Other files of the same game are noted on it.
    std::set<u64> inFolders, madeFromFolders;
    std::map<u64, std::vector<std::string>> alsoAsFiles;
    for (auto& path : paths) {
        auto found = _fileIndex.find(path);
        if (found != _fileIndex.end() && found->second.extracted && !found->second.titles.empty())
            inFolders.insert(found->second.titles[0].id);
    }
    for (auto& path : paths) {
        auto found = _fileIndex.find(path);
        if (found == _fileIndex.end() || found->second.extracted || !containsNoCase(path, "[LZ4]"))
            continue;
        for (auto& title : found->second.titles) {
            if (inFolders.count(title.id))
                madeFromFolders.insert(title.id);
        }
    }

    for (auto& path : paths) {
        auto found = _fileIndex.find(path);
        if (found == _fileIndex.end())
            continue;
        const IndexedFile& file = found->second;
        if (file.extracted) {
            if (!file.titles.empty())
                rows.push_back(rowForExtracted(path, file.titles[0], madeFromFolders.count(file.titles[0].id) > 0));
            continue;
        }
        for (auto& title : file.titles) {
            if (inFolders.count(title.id) || inFolders.count(title.linked)) {
                if (!madeFromFolders.count(title.id))
                    alsoAsFiles[title.type == Cnmt::MetaApplication ? title.id : title.linked].push_back("它也位于 '" + fileNameOf(path) + "'");
                continue;
            }
            IndexedCopy copy{&path, &file, &title};
            if (title.type == Cnmt::MetaApplication)
                bases.push_back(copy);
            else if (title.type == Cnmt::MetaPatch)
                updates.push_back(copy);
            else if (title.type == Cnmt::MetaAddOnContent)
                dlcs.push_back(copy);
        }
    }

    // Games in the order they're first come across, each with whatever there is of it.
    std::vector<u64> order;
    std::map<u64, std::vector<IndexedCopy>> basesOf;
    std::map<u64, std::map<u64, std::vector<IndexedCopy>>> dlcOf;
    auto note = [&](u64 application) {
        if (!basesOf.count(application) && !dlcOf.count(application))
            order.push_back(application);
    };
    for (auto& base : bases) {
        note(base.title->id);
        basesOf[base.title->id].push_back(base);
    }
    for (auto& dlc : dlcs) {
        note(dlc.title->linked);
        dlcOf[dlc.title->linked][dlc.title->id].push_back(dlc);
    }
    for (u64 application : order) {
        std::vector<std::string> notes;
        IndexedCopy main, update;
        bool compressed = true;
        bool hasBase = basesOf.count(application) > 0;
        if (hasBase) {
            std::vector<IndexedCopy> updatesFor;
            for (auto& candidate : updates) {
                if (candidate.title->linked == application)
                    updatesFor.push_back(candidate);
            }
            compressed = chooseCopy(basesOf[application], updatesFor, "本体游戏", notes, main, update);
        }
        std::vector<IndexedCopy> chosenDlcs;
        for (auto& dlc : dlcOf[application]) {
            std::vector<std::string> dlcNotes;
            IndexedCopy chosen, none;
            compressed = chooseCopy(dlc.second, {}, "DLC", dlcNotes, chosen, none) && compressed;
            for (auto& text : dlcNotes)
                notes.push_back(format("DLC %016llX: ", (unsigned long long)dlc.first) + text);
            chosenDlcs.push_back(chosen);
        }
        if (!hasBase) {
            main = chosenDlcs[0];
            chosenDlcs.erase(chosenDlcs.begin());
        }
        rows.push_back(rowForGame(main, update.title ? &update : nullptr, chosenDlcs, std::move(notes), compressed));
    }
    for (auto& row : rows) {
        if (!row.extracted.empty() && alsoAsFiles.count(row.application))
            row.notes = alsoAsFiles[row.application];
    }
    return rows;
}

/// Which of copies just as new to use, by where it is: the SD card, then a USB drive, then a PC on the cable, then one over the network.
int App::placeRank(const std::string& path) {
    if (remote::isRemote(path))
        return startsWithNoCase(path, "pc:USB/") ? 2 : 3;
    for (auto& drive : _drives) {
        if (startsWithNoCase(path, drive.root))
            return drive.isUsb ? 1 : 0;
    }
    return 0;
}

/// The newest of the copies, and of copies just as new, the nearest by placeRank, and then one that isn't packed with NCZ, as that takes
/// longer to read. The others are noted.
App::IndexedCopy App::newestCopy(std::vector<IndexedCopy> copies, const char* kind, std::vector<std::string>& notes) {
    auto isNcz = [](const IndexedCopy& c) { return endsWithNoCase(*c.path, ".nsz") || endsWithNoCase(*c.path, ".xcz"); };
    std::stable_sort(copies.begin(), copies.end(), [&](const IndexedCopy& a, const IndexedCopy& b) {
        if (a.title->version != b.title->version)
            return a.title->version > b.title->version;
        if (placeRank(*a.path) != placeRank(*b.path))
            return placeRank(*a.path) < placeRank(*b.path);
        return !isNcz(a) && isNcz(b);
    });
    for (size_t i = 1; i < copies.size(); i++) {
        std::string where = fileNameOf(*copies[i].path);
        notes.push_back(copies[i].title->version == copies[0].title->version ? format("Another copy of this %s is in '%s'", kind, where.c_str())
                                                                             : format("An older %s, v%u, is in '%s'", kind, copies[i].title->version, where.c_str()));
    }
    return copies[0];
}

/// Which copy of a title, and of its update, goes on the list. What this tool already compressed takes the place of what it was made from
/// when it's as new, and then there's no update to go with it. True if what's chosen is compressed.
bool App::chooseCopy(const std::vector<IndexedCopy>& all, const std::vector<IndexedCopy>& updatesFor, const char* kind, std::vector<std::string>& notes, IndexedCopy& main, IndexedCopy& update) {
    std::vector<IndexedCopy> originals, compressed;
    for (auto& copy : all)
        (isOwnOutput(*copy.path) ? compressed : originals).push_back(copy);
    update = IndexedCopy();
    u32 madeFrom = 0;
    if (!originals.empty()) {
        main = newestCopy(originals, kind, notes);
        if (!updatesFor.empty()) {
            IndexedCopy candidate = newestCopy(updatesFor, "update", notes);
            bool goesWith = candidate.title->version > main.title->version || (candidate.title->version == main.title->version && main.title->holdsOnlyPart);
            if (goesWith)
                update = candidate;
            else
                notes.push_back(format("Its update v%u in '%s' was left aside, as the base game is already v%u", candidate.title->version, fileNameOf(*candidate.path).c_str(), main.title->version));
        }
        madeFrom = std::max(main.title->version, update.title ? update.title->version : 0u);
    }
    std::stable_sort(compressed.begin(), compressed.end(), [&](const IndexedCopy& a, const IndexedCopy& b) {
        if (a.title->version != b.title->version)
            return a.title->version > b.title->version;
        return placeRank(*a.path) < placeRank(*b.path);
    });
    if (!compressed.empty() && (originals.empty() || compressed[0].title->version >= madeFrom)) {
        for (size_t i = 1; i < compressed.size(); i++)
            notes.push_back(format("Another compressed copy, v%u, is in '%s'", compressed[i].title->version, fileNameOf(*compressed[i].path).c_str()));
        if (!originals.empty()) {
            std::string from = "'" + fileNameOf(*main.path) + "'" + (update.title ? "和 '" + fileNameOf(*update.path) + "'" : std::string());
            notes.insert(notes.begin(), "压缩自" + from + "，也位于此处");
        }
        main = compressed[0];
        update = IndexedCopy();
        return true;
    }
    for (auto& older : compressed)
        notes.push_back(format("An older compressed copy, v%u, is in '%s'. Compressing again makes one of v%u", older.title->version, fileNameOf(*older.path).c_str(), madeFrom));
    return false;
}

App::Row App::rowForExtracted(const std::string& path, const IndexedTitle& title, bool done) {
    Row row;
    row.tab = TabFiles;
    row.key = path;
    row.fingerprint = format("%lld", (long long)title.bytes);
    row.name = title.name;
    row.detail = joined({title.version ? format("v%u", title.version) : "", fileNameOf(path), whereIs(path)}, Dot);
    row.tile = tileColor(row.name);
    row.now = title.bytes;
    row.application = title.id;
    row.gameVersion = title.version;
    row.extracted = path;
    row.tiny = title.bytes < SmallGameBytes;
    row.icon = title.icon;
    if (done) {
        row.state = RowState::Done;
        row.ticked = false;
    }
    return row;
}

App::Row App::rowForGame(const IndexedCopy& main, const IndexedCopy* update, const std::vector<IndexedCopy>& dlcs, std::vector<std::string> notes, bool compressed) {
    const IndexedTitle& title = *main.title;
    Row row;
    row.tab = TabFiles;
    row.key = *main.path + format("#%016llx", (unsigned long long)title.id);
    row.titleId = title.id;
    row.mainFile = *main.path;
    i64 bytes = title.bytes;
    if (update) {
        row.updateFile = *update->path;
        // Together in a file with nothing else in it, the whole file is theirs.
        bytes = update->path == main.path && main.file->titles.size() == 2 ? main.file->size : bytes + update->title->bytes;
    }
    for (auto& dlc : dlcs) {
        row.dlcFiles.push_back(*dlc.path);
        row.dlcIds.push_back(dlc.title->id);
        bytes += dlc.title->bytes;
    }
    row.notes = std::move(notes);
    bool isBase = title.type == Cnmt::MetaApplication;
    row.fingerprint = format("%lld", (long long)bytes);
    std::string fileName = withoutExtension(fileNameOf(*main.path));
    size_t bracket = fileName.find('[');
    std::string dlcName = bracket != std::string::npos && bracket > 0 ? fileName.substr(0, bracket) : fileName;
    while (!dlcName.empty() && dlcName.back() == ' ')
        dlcName.pop_back();
    dlcName += "（DLC）";
    row.name = update && !update->title->name.empty() ? update->title->name : !title.name.empty() ? title.name : isBase ? fileName : dlcName;
    std::string what = !isBase ? format("DLC v%u", title.version)
                     : compressed || isOwnOutput(*main.path) ? format("compressed, v%u", title.version)
                     : update ? format("base v%u + update v%u, repacked as one title", title.version, update->title->version)
                              : format("base v%u", title.version);
    // A row of DLC alone already says it's DLC.
    int dlcCount = (int)dlcs.size() + (isBase ? 0 : 1);
    bool sayDlc = isBase ? dlcCount > 0 : dlcCount > 1;
    row.detail = joined({what, sayDlc ? format("%d DLC", dlcCount) : "", fileNameOf(*main.path), whereIs(*main.path)}, Dot);
    row.tile = tileColor(row.name);
    row.now = bytes;
    row.application = isBase ? title.id : title.linked;
    row.tiny = isBase && !update && bytes < SmallGameBytes;
    row.hasUpdate = update != nullptr;
    row.gameVersion = update ? std::max(title.version, update->title->version) : title.version;
    row.version = update && !update->title->displayVersion.empty() ? update->title->displayVersion : title.displayVersion;
    row.icon = update && update->title->icon ? update->title->icon : title.icon;
    if (title.holdsOnlyPart && !update)
        row.blocked = "这只包含游戏的一部分，其余内容在升级补丁里";
    // What this tool made, as new as what it was made from, is compressed, without having to read it to know. All that's left is
    // installing it, if game files get installed.
    if (compressed) {
        row.hasEstimate = true;
        row.estimate.inputBytes = row.estimate.outputBytes = bytes;
        row.estimate.alreadyCompressed = true;
        row.ticked = installsGameFiles();
    }
    return row;
}

// -------------------------------------------------------------------------------------------------------------------
// Game files: opening them, once there's something to do with them

/// Opened only for as long as that takes. A file that's open can't be deleted or written over.
App::OpenedJob App::openJob(const Row& row) {
    std::string files = row.mainFile + "|" + row.updateFile;
    for (auto& dlc : row.dlcFiles)
        files += "|" + dlc;
    if (!_openSet || _openSetFiles != files) {
        _openSet.reset();
        auto set = std::make_shared<TitleSet>(_keyset);
        std::set<std::string> added;
        for (auto* path : {&row.mainFile, &row.updateFile}) {
            if (!path->empty() && added.insert(*path).second)
                set->add(std::make_shared<FileArchive>(*path));
        }
        for (auto& dlc : row.dlcFiles) {
            if (added.insert(dlc).second)
                set->add(std::make_shared<FileArchive>(dlc));
        }
        set->group(true);
        _openSet = set;
        _openSetFiles = files;
    }
    for (auto& job : _openSet->jobs) {
        if (job.main->meta.titleId == row.titleId && job.main->archive->path() == row.mainFile)
            return OpenedJob{_openSet, &job};
    }
    std::string why = !_openSet->problems.empty() ? _openSet->problems[0].message : std::string("此文件与上次查看时不同，请刷新列表以查看当前内容");
    closeOpenJob();
    throw ToolError(why);
}

App::OpenedJobs App::openJobs(const Row& row) {
    OpenedJob main = openJob(row);
    OpenedJobs opened{main.set, {main.job}};
    for (size_t i = 0; i < row.dlcFiles.size(); i++) {
        const TitleJob* found = nullptr;
        for (auto& job : main.set->jobs) {
            if (job.main->meta.titleId == row.dlcIds[i] && job.main->archive->path() == row.dlcFiles[i])
                found = &job;
        }
        if (!found)
            throw ToolError("'" + fileNameOf(row.dlcFiles[i]) + "' 与上次查看时不同，请刷新列表以查看当前内容");
        opened.jobs.push_back(found);
    }
    return opened;
}

void App::closeOpenJob() {
    _openSet.reset();
    _openSetFiles.clear();
}

/// Going through a folder with thousands of files in it only needs doing once.
ExtractedGame App::openExtracted(const std::string& path) {
    {
        sys::Lock lock(_mutex);
        if (_openExtracted && _openExtractedPath == path)
            return *_openExtracted;
    }
    auto game = std::make_shared<ExtractedGame>(ExtractedGame::open(path));
    sys::Lock lock(_mutex);
    _openExtracted = game;
    _openExtractedPath = path;
    return *game;
}

// -------------------------------------------------------------------------------------------------------------------
// What's worth it, what's done, and the order

void App::judgeLocked(Row& row, const ConvertOptions& options) {
    row.belowMinimum.clear();
    if (row.hasEstimate && options.compress) {
        // A compressed game file can still be installed, which is all that's left to do with it.
        if (row.estimate.alreadyCompressed) {
            if (!compressedToInstallLocked(row))
                row.belowMinimum = "Already compressed";
        } else if (row.estimate.savedBytes() < 0) {
            row.belowMinimum = format("Nothing to gain: it would grow by %.0f%%", -row.estimate.savedFraction() * 100);
        } else if (row.estimate.savedFraction() < NothingToGain) {
            row.belowMinimum = "没有收益：已经是最小的了";
        } else if (!SavingRule::worthCompressing(row.estimate, options)) {
            row.belowMinimum = "已排除：它" + SavingRule::reason(row.estimate, options);
        }
    }
    if (!row.byHand)
        row.ticked = row.belowMinimum.empty() && row.blocked.empty() && row.state != RowState::Done && row.state != RowState::Failed;
}

void App::judgeAllLocked() {
    ConvertOptions options = convertOptions();
    for (auto& row : _rows)
        judgeLocked(row, options);
    _dirty = true;
}

void App::sortRowsLocked() {
    // Whatever has the focus stays under the eye, unless it was at the top, which is where the best one now is.
    std::string focused[TabCount];
    int seen[TabCount] = {0, 0, 0};
    for (auto& row : _rows) {
        if (leftOffListLocked(row))
            continue;
        if (seen[row.tab]++ == _focus[row.tab] && _focus[row.tab] > 0)
            focused[row.tab] = row.key;
    }

    int sort = _settings.sort;
    std::stable_sort(_rows.begin(), _rows.end(), [&](const Row& a, const Row& b) {
        if (a.tab != b.tab)
            return a.tab < b.tab;
        // What's been done is out of the way of what's still to do.
        bool aDone = isCompressed(a), bDone = isCompressed(b);
        if (aDone != bDone)
            return bDone;
        if (sort == 0 && _sortedByGain[a.tab]) {
            // What gains the most for the time it takes, as on the other versions.
            if (a.hasEstimate != b.hasEstimate)
                return a.hasEstimate;
            if (a.hasEstimate && a.estimate.savedFraction() != b.estimate.savedFraction())
                return a.estimate.savedFraction() > b.estimate.savedFraction();
        } else if (sort == 1 && a.now != b.now) {
            return a.now > b.now;
        }
        return lessNoCase(a.name, b.name);
    });

    seen[0] = seen[1] = seen[2] = 0;
    for (auto& row : _rows) {
        if (leftOffListLocked(row))
            continue;
        if (row.key == focused[row.tab])
            _focus[row.tab] = seen[row.tab];
        seen[row.tab]++;
    }
}

App::Row* App::findLocked(const std::string& key) {
    auto found = std::find_if(_rows.begin(), _rows.end(), [&](const Row& r) { return r.key == key; });
    return found == _rows.end() ? nullptr : &*found;
}

bool App::isCompressed(const Row& row) const {
    return row.state == RowState::Done || (row.state == RowState::Ready && row.hasEstimate && row.estimate.alreadyCompressed && _settings.compress);
}

/// A game file whose game is already installed compressed, as new or newer. What the console has recorded as the version can be changed,
/// as DBI's reset of the required version does, so the version the game itself gives, such as 1.0.2, being the same counts too.
bool App::installedCompressedLocked(const Row& row) const {
    if (row.tab != TabFiles || !_console || !_settings.compress)
        return false;
    // DLC on its own is for a game, not the game itself.
    if (row.extracted.empty() && row.titleId != row.application)
        return false;
    for (auto& installed : _rows) {
        if (installed.tab == TabInstalled && installed.application == row.application && installed.ready)
            return isCompressed(installed) && (installed.gameVersion >= row.gameVersion || (!row.version.empty() && row.version == installed.version));
    }
    return false;
}

/// Whether there's anything left to do with a game, which is what Hide games that are done goes by. An installed game is done once it's
/// compressed. A game file is done once its game is installed compressed, when game files are installed, as a compressed file that
/// isn't installed yet still has that to do. When they're saved as NSPs instead, it's done once it's compressed.
bool App::isDoneLocked(const Row& row) const {
    if (row.state == RowState::Done)
        return true;
    if (row.tab == TabFiles && installsGameFiles())
        return installedCompressedLocked(row);
    return isCompressed(row);
}

/// A compressed game file that's to be installed, and isn't yet, which only takes copying it across.
bool App::compressedToInstallLocked(const Row& row) const {
    return row.tab == TabFiles && installsGameFiles() && isCompressed(row) && !isDoneLocked(row);
}

bool App::hiddenAsDoneLocked(const Row& row) const { return _settings.hideCompressed && isDoneLocked(row); }

bool App::leftOffListLocked(const Row& row) const { return (_settings.hideSmall && row.tiny) || hiddenAsDoneLocked(row); }

int App::doneHiddenLocked(int tab) const {
    int hidden = 0;
    for (auto& row : _rows)
        hidden += row.tab == tab && hiddenAsDoneLocked(row) ? 1 : 0;
    return hidden;
}

std::vector<App::Row*> App::visibleRowsLocked(int tab) {
    std::vector<Row*> rows;
    for (auto& row : _rows) {
        if (row.tab == tab && !leftOffListLocked(row))
            rows.push_back(&row);
    }
    return rows;
}

std::vector<std::string> App::keysToAnalyzeLocked(int tab) {
    std::vector<std::string> keys;
    for (auto* row : visibleRowsLocked(tab)) {
        if (row->ready && row->ticked && !row->hasEstimate && row->blocked.empty() && row->state == RowState::Ready)
            keys.push_back(row->key);
    }
    return keys;
}

}  // namespace hbp
