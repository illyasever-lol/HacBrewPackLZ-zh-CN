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

bool App::isCardPath(const std::string& path) { return path.compare(0, 9, "gamecard:") == 0; }

namespace {

/// A game card's game, read through the console, as the game file the list has it as: its path is the card's, which is how a job finds the
/// file it was listed from.
class CardArchive : public InstalledArchive {
public:
    CardArchive(ConsoleApi& console, const InstalledGame& game, const std::string& path) : InstalledArchive(console, game) { _path = path; }
};

}  // namespace

std::string App::cardPath(u64 application) { return format("gamecard:%016llx", (unsigned long long)application); }

InstalledGame App::cardGame(u64 application) {
    InstalledGame game = _console->describe(application);
    game.metas.erase(std::remove_if(game.metas.begin(), game.metas.end(), [](const InstalledMeta& meta) { return meta.storage != Storage::GameCard; }), game.metas.end());
    return game;
}

std::vector<std::pair<std::string, i64>> App::gameCardFiles() {
    std::vector<std::pair<std::string, i64>> files;
    // With one folder chosen, that's all that's looked in.
    if (!_console || !_settings.fromGameCard || !_settings.filesFolder.empty())
        return files;
    try {
        for (u64 application : _console->cardApplications()) {
            InstalledGame game = cardGame(application);
            i64 bytes = 0;
            for (auto& meta : game.metas) {
                for (auto& content : meta.contents)
                    bytes += content.size;
            }
            if (bytes > 0)
                files.emplace_back(cardPath(application), bytes);
        }
    } catch (const ToolError& e) {
        logLine(std::string("无法读取游戏卡带：") + e.what());
    }
    return files;
}

ArchivePtr App::archiveFor(const std::string& path) {
    if (!isCardPath(path))
        return std::make_shared<FileArchive>(path);
    if (!_console)
        throw ToolError("没有可用于读取卡带的主机");
    InstalledGame game = cardGame(strtoull(path.c_str() + strlen("gamecard:"), nullptr, 16));
    bool readable = std::any_of(game.metas.begin(), game.metas.end(), [](const InstalledMeta& meta) { return !meta.contents.empty(); });
    if (!readable)
        throw ToolError("无法读取游戏卡带，是否仍然插着？");
    return std::make_shared<CardArchive>(*_console, game, path);
}

/// Where a game file is, to say so on the list when it looks in more than one place.
std::string App::whereIs(const std::string& path) {
    if (isCardPath(path))
        return "在 游戏卡带";
    if (remote::isRemote(path)) {
        std::string id = path.substr(3, path.find('/') == std::string::npos ? std::string::npos : path.find('/') - 3);
        return id == "USB" ? "在 通过 USB 数据线连接的电脑" : "在 " + id;
    }
    for (auto& drive : _drives) {
        if (startsWithNoCase(path, drive.root))
            return drive.isUsb ? "在 " + drive.name : "在 SD 卡上";
    }
    return "";
}

/// The list of game files gets looked through again the next time it's due, from the folders up.
void App::noteSaved(const std::string& path) {
    sys::Lock lock(_mutex);
    _savedFiles.push_back(path);
}

/// What a job saved goes on the list of game files without going through every folder again for it, where the folders have been
/// gone through and it's somewhere that's looked in. Anything else, the list is looked for again.
void App::addSavedFiles() {
    std::vector<std::string> roots = fileRoots();
    sys::Lock lock(_mutex);
    std::vector<std::string> saved;
    saved.swap(_savedFiles);
    bool all = _found.walked;
    for (auto& path : saved) {
        if (!all || !isWithinAny(path, roots) || !sys::exists(path)) {
            all = false;
            break;
        }
    }
    if (!all) {
        _found.walked = false;
    } else {
        for (auto& path : saved) {
            _found.files.erase(std::remove_if(_found.files.begin(), _found.files.end(), [&](const std::pair<std::string, i64>& f) { return f.first == path; }), _found.files.end());
            _found.files.emplace_back(path, sys::fileSize(path));
        }
    }
    _scanned[TabFiles] = false;
    _openExtracted.reset();
    _openExtractedPath.clear();
}

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
    bool recalled = recallEstimateLocked(row);
    if (recalled)
        _sortedByGain[row.tab] = true;
    else if (!row.blocked.empty() || (row.state == RowState::Failed && !row.byHand))
        row.ticked = false;
    _listChanged = _listChanged || row.tab == TabInstalled;
    _rows.push_back(std::move(row));
    // A compressed file is known to be one without being analyzed, and is judged on that, as it's on the list with how its game is
    // installed. A game that's installed decides what's left to do with its files.
    Row& added = _rows.back();
    if (recalled || added.hasEstimate)
        judgeLocked(added, convertOptions());
    if (added.tab == TabInstalled)
        judgeFilesOfLocked(added.application);
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
    row.updateOverCopy = game.updateOverCopy;
    row.markedIncomplete = game.markedIncomplete;
    row.asksSystem = game.asksSystem;
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
    row.hasDlc = dlc > 0;

    std::string version = row.version.empty() ? "" : "版本 " + row.version;
    if (hasUpdate)
        version += version.empty() ? "含更新补丁" : " 及其更新补丁";
    row.detail = joined({version, dlc == 0 ? "" : format("%d DLC", dlc), row.storage == Storage::Nand ? "主机上" : row.storage == Storage::Sd ? "SD 卡上" : ""}, Dot);

    if (row.blocked.empty() && !hasBase)
        row.blocked = "未安装其本体游戏，因此无法重建";
    if (row.blocked.empty() && row.storage == Storage::GameCard)
        row.blocked = "位于游戏卡带上：请从「游戏文件」安装";
}

/// Why a game asks for an update before it starts is something the console keeps to itself, so the log says, once each time the app opens:
/// every game the console won't start as it is, as it wants a newer version than the one installed (a newer one was started before, or its
/// version list requires it), and every compressed game it knows a newer version of, which it offers first, along with Start Software.
void App::logVersions(const std::vector<InstalledVersion>& installed) {
    std::vector<u64> ids;
    for (auto& game : installed)
        ids.push_back(game.application);
    std::vector<VersionFigures> figures;
    try {
        figures = _console->versionFigures(ids);
    } catch (const ToolError& e) {
        logLine(std::string("Couldn't ask the console which versions of its games it wants: ") + e.what());
        return;
    }
    for (size_t i = 0; i < figures.size() && i < installed.size(); i++) {
        const InstalledVersion& game = installed[i];
        const VersionFigures& wanted = figures[i];
        bool wontStart = wanted.launchRequired > game.version || (wanted.listed && wanted.required > game.version);
        if (!wontStart && !(wanted.listed && wanted.newest > game.version))
            continue;
        // Named as on the list, where a game new to it isn't yet, and by its title ID, which a tester can look up.
        std::string name;
        {
            sys::Lock lock(_mutex);
            Row* row = findLocked(format("%016llx", (unsigned long long)game.application));
            if (!wontStart && (!row || !isCompressed(*row)))
                continue;
            name = row && !row->name.empty() ? row->name + " " : std::string();
        }
        std::string list = wanted.listed ? format("its version list has v%u as the newest and v%u as required", wanted.newest, wanted.required) : std::string("it isn't in its version list");
        logLine(format("%s[%016llX]: v%u installed, and the console %s: it wants v%u to start it, and %s", name.c_str(), (unsigned long long)game.application,
                       game.version, wontStart ? "won't start it as it is" : "offers a newer version before starting it", wanted.launchRequired, list.c_str()));
    }
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
        std::set<u64> gone;
        for (auto& r : _rows) {
            if (r.tab == TabInstalled && !present.count(r.key))
                gone.insert(r.application);
        }
        _rows.erase(std::remove_if(_rows.begin(), _rows.end(), [&](const Row& r) { return r.tab == TabInstalled && !present.count(r.key); }), _rows.end());
        _listChanged = _listChanged || _rows.size() != before;
        if (!gone.empty())
            installedGoneLocked(gone);
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
            judgeUpdateOver(described[i]);
            judgeMark(described[i]);
            readable[i] = 1;
        } catch (const ToolError& e) {
            logLine(format("%016llx can't be read: %s", (unsigned long long)list[i], e.what()));
        }
        sys::Lock lock(_mutex);
        _jobIndex = ++gone;
    });

    // The versions installed, taken before what's new is moved on, for the log once the list says what's compressed.
    std::vector<InstalledVersion> versions;
    for (size_t i = 0; i < list.size() && !_versionsLogged; i++) {
        if (!readable[i] || described[i].totalBytes() == 0)
            continue;
        InstalledVersion installed{list[i], 0};
        for (auto& meta : described[i].metas) {
            if (meta.key.type == Cnmt::MetaApplication || meta.key.type == Cnmt::MetaPatch)
                installed.version = std::max(installed.version, meta.key.version);
        }
        versions.push_back(installed);
    }

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
                u64 application = known->application;
                _rows.erase(_rows.begin() + (known - _rows.data()));
                _listChanged = true;
                installedGoneLocked({application});
            }
            continue;
        }
        if (_hidden.count(key))
            continue;
        if (known && known->fingerprint == fingerprintOf(game) && !known->name.empty()) {
            std::string before = known->detail;
            applyContents(*known, game);
            // What the version before worked out for it waits until now, when it's known what the game has with it.
            if (!known->hasEstimate && known->state == RowState::Ready && recallEstimateLocked(*known))
                _sortedByGain[TabInstalled] = true;
            judgeLocked(*known, convertOptions());
            judgeFilesOfLocked(known->application);
            _listChanged = _listChanged || known->detail != before;
        } else {
            toRead.push_back(std::move(game));
        }
    }
    logLine(format("%d games installed, %d of them not seen before, gone over in %.1f s", (int)list.size(), (int)toRead.size(), sys::now() - started));
    if (!_versionsLogged) {
        _versionsLogged = true;
        logVersions(versions);
    }

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

    // An update file for a game that's installed is only listed once the game's known to be installed, which may be after the game
    // files were looked through. The list is made from what it was last made from, with what this made in an NSP folder elsewhere.
    sys::Lock lock(_mutex);
    if (_scanned[TabFiles])
        showIndexRowsLocked(_listedPaths, true);
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
    // What Look inside saved from inside games is never a game, however much of one is there.
    skip.push_back(filesFromGamesFolder());
    // Folders chosen to look in are looked through all the way down, as that's where the games were said to be.
    std::vector<std::string> apps = _settings.filesFolder.empty() ? _platform.appFolders() : std::vector<std::string>();
    auto deepInAnApp = [&](const std::string& path) {
        for (auto& app : apps) {
            if (!startsWithNoCase(path, app + "/"))
                continue;
            // The app's own folder and what's straight inside it are looked at, as a folder of games can be kept there, but nothing deeper.
            std::string within = path.substr(app.size() + 1);
            return std::count(within.begin(), within.end(), '/') >= 2;
        }
        return false;
    };
    int left = 0;
    std::vector<std::string> level;
    for (auto& root : roots) {
        if (sys::isDirectory(root) && std::find(level.begin(), level.end(), root) == level.end())
            level.push_back(root);
    }

    int folders = 0;
    std::set<std::string> seen(level.begin(), level.end());
    sys::Mutex collecting;
    // Folders the system kept turning down, which are tried again once the rest have been gone through, and named in the log if they still are.
    std::vector<std::string> turnedDown;
    bool triedAgain = false;
    while (!level.empty()) {
        logLine(format("Looking through %d folders, starting with %s", (int)level.size(), level[0].c_str()));
        folders += (int)level.size();
        std::vector<std::string> next;
        // A few folders at once, as each one listed is mostly waiting for the card. It was two after custom firmware's fs.mitm was seen to fall
        // over, from requests turned down for short path buffers, which are full-size since.
        constexpr int AtOnce = 4;
        parallelFor(AtOnce, (int)level.size(), [&](int index, int) {
            checkScanStop();
            const std::string& folder = level[(size_t)index];
            bool refused = false;
            std::vector<sys::DirEntry> entries = sys::list(folder, false, &refused);
            if (refused) {
                sys::Lock lock(collecting);
                turnedDown.push_back(folder);
                return;
            }
            auto has = [&](const char* name) { return std::any_of(entries.begin(), entries.end(), [&](const sys::DirEntry& e) { return e.isDirectory && equalsNoCase(e.name, name); }); };

            // What's in the folder of an extracted game is that game, whatever else it may look like.
            if (has("exefs") && has("control")) {
                // An extraction that didn't finish leaves its note there, and is only part of a game, however much of one it looks like.
                bool unfinished = std::any_of(entries.begin(), entries.end(), [](const sys::DirEntry& e) { return !e.isDirectory && equalsNoCase(e.name, ExtractedGame::NotFinished); });
                i64 npdm = unfinished ? -1 : sys::fileSize(folder + "/exefs/main.npdm"), nacp = unfinished ? -1 : sys::fileSize(folder + "/control/control.nacp");
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
                    // Hidden folders are an app's or a computer's own, as with .packages or .Trashes.
                    bool leftOut = entry.name[0] == '.' || deepInAnApp(inside) || std::any_of(skip.begin(), skip.end(), [&](const std::string& s) { return equalsNoCase(inside, s); });
                    if (leftOut) {
                        sys::Lock lock(collecting);
                        left++;
                    } else {
                        within.push_back(std::move(inside));
                    }
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
        // The system doesn't go on turning a folder down for ever, so those are tried once more at the end.
        if (level.empty() && !triedAgain && !turnedDown.empty()) {
            triedAgain = true;
            logLine(format("Trying %d folders again that the system turned down", (int)turnedDown.size()));
            folders -= (int)turnedDown.size();
            level = std::move(turnedDown);
            turnedDown.clear();
        }
    }
    if (!turnedDown.empty()) {
        std::sort(turnedDown.begin(), turnedDown.end());
        std::vector<std::string> named(turnedDown.begin(), turnedDown.begin() + std::min<size_t>(turnedDown.size(), 5));
        logLine(format("Couldn't look in %d folders, as the system kept turning them down, so game files in them aren't on the list: %s", (int)turnedDown.size(),
                       joined(named, ", ").c_str()));
    }

    auto byName = [](const std::pair<std::string, i64>& a, const std::pair<std::string, i64>& b) { return lessNoCase(a.first, b.first); };
    std::sort(found.files.begin(), found.files.end(), byName);
    std::sort(found.extracted.begin(), found.extracted.end(), byName);
    logLine(format("Went through %d folders, left out %d that only apps keep their own things in, and found %d game files and %d extracted games", folders, left,
                   (int)found.files.size(), (int)found.extracted.size()));
    return found;
}

/// What it takes to put a game file or an extracted game on the list, which is a few small reads and an icon to decode.
/// A problem that was only passing, such as the card turning a request down, or a folder it wouldn't let all of be read: looked at again the
/// next time rather than remembered, which kept a game that could be read unusable until its file changed.
static bool passingProblem(const std::string& problem) {
    return !problem.empty() && (refusedBySystem(problem) || problem.find("Try again in a moment") != std::string::npos);
}

App::IndexedFile App::readIndexEntry(const std::string& path, i64 size, bool extracted) {
    IndexedFile entry;
    entry.size = size;
    entry.extracted = extracted;
    // A PC that's sharing a file can say what's in it far faster than reading it from here, over the network, would.
    if (remote::isRemote(path) && fromPc(path, entry))
        return entry;
    logLine("正在打开 " + path);
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
            set.add(archiveFor(path));
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
                title.madeHere = summary.madeHere;
                title.originalBytes = summary.originalBytes;
                title.solidBytes = summary.solidBytes;
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
            // An earlier PC version leaves out the display version, and one before this, whether this tool made it, which then goes by
            // the file's name as it used to.
            if (fields.size() >= 10) {
                title.displayVersion = fields[8];
                title.name = fields[9];
            } else {
                title.name = fields[8];
            }
            if (fields.size() >= 12) {
                title.madeHere = fields[10] == "1";
                title.originalBytes = atoll(fields[11].c_str());
            } else {
                title.madeHere = isOwnOutput(path);
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
    // A game card that's in is looked at each time, as it can be taken out and another put in at any time.
    std::vector<std::pair<std::string, i64>> cardFiles = gameCardFiles();

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
            if (known == _fileIndex.end() || known->second.size != item.second || known->second.extracted != extracted || passingProblem(known->second.problem))
                toRead.push_back(ToRead{item.first, item.second, extracted});
        };
        for (auto& file : _found.files)
            consider(file, false);
        for (auto& game : _found.extracted)
            consider(game, true);
        for (auto& card : cardFiles)
            consider(card, false);
        // What's gone from where it was isn't anything to know about any more, a game card taken out included.
        for (auto it = _fileIndex.begin(); it != _fileIndex.end();) {
            if ((isWithinAny(it->first, roots) || isCardPath(it->first)) && !there.count(it->first)) {
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

    copiesInNspFolder(roots, paths);
    sys::Lock lock(_mutex);
    showIndexRowsLocked(paths, true);
    logLine(format("%d game files and folders on this list, %d of them opened, gone over in %.2f s", (int)paths.size(), (int)toRead.size(), sys::now() - started));
    logWhatsDoneLocked();
}

/// With one folder chosen, what this made of its games where NSPs are saved, elsewhere, goes on the list too, as a copy saved beside its original
/// would: otherwise a game compressed there would be to do again the next time this opens. Only copies of its games come, found by the names
/// this gives them, without opening anything else: a game's has its title ID and [LZ4] in it, and DLC's is its own file's name with [LZ4] after.
void App::copiesInNspFolder(const std::vector<std::string>& roots, std::vector<std::string>& paths) {
    std::string folder = nspFolder();
    if (_settings.filesFolder.empty() || isWithinAny(folder, roots) || !sys::isDirectory(folder))
        return;
    std::set<std::string> ids, names;
    {
        sys::Lock lock(_mutex);
        for (auto& path : paths) {
            auto found = _fileIndex.find(path);
            if (found == _fileIndex.end())
                continue;
            for (auto& title : found->second.titles) {
                ids.insert(format("[%016llX]", (unsigned long long)title.id));
                if (title.linked)
                    ids.insert(format("[%016llX]", (unsigned long long)title.linked));
            }
            std::string stem = fileNameOf(path);
            stem = stem.substr(0, stem.rfind('.'));
            if (!stem.empty())
                names.insert(stem + (stem.back() == ']' ? "[LZ4].nsp" : " [LZ4].nsp"));
        }
    }
    std::string base = folder.back() == '/' ? folder : folder + "/";
    for (auto& entry : sys::list(folder)) {
        if (entry.isDirectory || !FileArchive::isGameFile(entry.name) || !containsNoCase(entry.name, "[LZ4]"))
            continue;
        // Saved beside something of the same name, it has a number after it ("… [LZ4] (2).nsp"), or with a version before 1.5, "(repacked)".
        std::string saved = entry.name;
        size_t dot = saved.rfind('.'), open = dot != std::string::npos && dot > 0 && saved[dot - 1] == ')' ? saved.rfind(" (", dot) : std::string::npos;
        if (open != std::string::npos)
            saved = saved.substr(0, open) + saved.substr(dot);
        bool ours = std::any_of(names.begin(), names.end(), [&](const std::string& name) { return equalsNoCase(name, entry.name) || equalsNoCase(name, saved); }) ||
                    std::any_of(ids.begin(), ids.end(), [&](const std::string& id) { return containsNoCase(entry.name, id); });
        std::string path = base + entry.name;
        if (!ours || std::find(paths.begin(), paths.end(), path) != paths.end())
            continue;
        paths.push_back(path);
        bool known;
        {
            sys::Lock lock(_mutex);
            auto found = _fileIndex.find(path);
            known = found != _fileIndex.end() && found->second.size == entry.size && !found->second.extracted && !passingProblem(found->second.problem);
        }
        if (known)
            continue;
        IndexedFile read = readIndexEntry(path, entry.size, false);
        sys::Lock lock(_mutex);
        _fileIndex[path] = std::move(read);
        _fileIndexChanged = true;
    }
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
    _listedPaths = paths;
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

    // A file the card turned down this time, or one not read yet, may be a DLC's own file: a refusal never means it isn't there.
    bool unsure = false;
    for (auto& path : paths) {
        auto found = _fileIndex.find(path);
        if (found == _fileIndex.end()) {
            unsure = true;
            continue;
        }
        const IndexedFile& file = found->second;
        unsure = unsure || passingProblem(file.problem);
        if (file.extracted) {
            if (!file.titles.empty())
                rows.push_back(rowForExtracted(path, file.titles[0], madeFromFolders.count(file.titles[0].id) > 0));
            continue;
        }
        for (auto& title : file.titles) {
            if (inFolders.count(title.id) || inFolders.count(title.linked)) {
                if (!madeFromFolders.count(title.id))
                    alsoAsFiles[title.type == Cnmt::MetaApplication ? title.id : title.linked].push_back("也位于「" + fileNameOf(path) + "」");
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
    // With Game and DLC in one file, while NSPs are saved, a file holds a game's DLC together when it has each one here, at least as new. A copy
    // of the game this tool made that doesn't is made again with them, and so are DLC without their game that aren't all in one file.
    bool together = _settings.dlcInGameFile && !installsGameFiles();
    for (u64 application : order) {
        std::vector<std::string> notes;
        IndexedCopy main, update;
        bool compressed = true;
        bool hasBase = basesOf.count(application) > 0;
        const auto& dlcsHere = dlcOf[application];
        // Each of its DLC with its newest version here, and what's known of each file, worked out once: a game can have dozens of DLC, and
        // this is done under the lock every few files while they're looked through.
        std::vector<std::pair<u64, u32>> newest;
        std::map<const IndexedFile*, bool> holding;
        std::function<bool(const IndexedFile&)> holdsAll;
        for (auto& dlc : dlcsHere) {
            u32 version = 0;
            for (auto& copy : dlc.second)
                version = std::max(version, copy.title->version);
            newest.emplace_back(dlc.first, version);
        }
        // A DLC has no file of its own when no copy of it at its newest version is alone in its file or in a file this tool made, as with one
        // only inside a merged original or another tool's DLC pack. Only headers say what this tool made, so its own file of DLC with nothing
        // to compress counts as none, which settles once they have one. Worked out from the index, writing nothing, and only once every file
        // has been read: until then nothing gives way or is made on its say.
        std::set<u64> withoutFile;
        for (auto& dlc : newest) {
            const auto& copies = dlcsHere.at(dlc.first);
            bool own = std::any_of(copies.begin(), copies.end(), [&](const IndexedCopy& copy) {
                const auto& titles = copy.file->titles;
                return copy.title->version >= dlc.second &&
                       (titles.size() == 1 || std::any_of(titles.begin(), titles.end(), [](const IndexedTitle& title) { return title.madeHere; }));
            });
            if (!own && !unsure)
                withoutFile.insert(dlc.first);
        }
        if (together) {
            holdsAll = [&newest, &holding](const IndexedFile& file) {
                auto known = holding.find(&file);
                if (known != holding.end())
                    return known->second;
                bool all = std::all_of(newest.begin(), newest.end(), [&](const std::pair<u64, u32>& dlc) {
                    return std::any_of(file.titles.begin(), file.titles.end(), [&](const IndexedTitle& title) {
                        return title.type == Cnmt::MetaAddOnContent && title.id == dlc.first && title.version >= dlc.second;
                    });
                });
                holding.emplace(&file, all);
                return all;
            };
        }
        if (hasBase) {
            std::vector<IndexedCopy> updatesFor;
            for (auto& candidate : updates) {
                if (candidate.title->linked == application)
                    updatesFor.push_back(candidate);
            }
            compressed = chooseCopy(basesOf[application], updatesFor, "本体游戏", notes, main, update, holdsAll, !withoutFile.empty());
        }
        std::vector<IndexedCopy> chosenDlcs;
        for (auto& dlc : dlcsHere) {
            std::vector<std::string> dlcNotes;
            IndexedCopy chosen, none;
            compressed = chooseCopy(dlc.second, {}, "DLC", dlcNotes, chosen, none, holdsAll) && compressed;
            for (auto& text : dlcNotes)
                notes.push_back(format("DLC %016llX: ", (unsigned long long)dlc.first) + text);
            chosenDlcs.push_back(chosen);
        }
        if (!hasBase) {
            main = chosenDlcs[0];
            chosenDlcs.erase(chosenDlcs.begin());
        }
        bool remake = together && (hasBase ? main.title->madeHere && !holdsAll(*main.file) : !chosenDlcs.empty());
        int apart = 0;
        for (auto& dlc : chosenDlcs)
            apart += *dlc.path != *main.path ? 1 : 0;
        remake = remake && apart > 0;
        rows.push_back(rowForGame(main, update.title ? &update : nullptr, chosenDlcs, std::move(notes), compressed && !remake, hasBase && remake ? apart : 0));
        rows.back().dlcNotInFile = remake;
        // With it off, while NSPs are saved, a DLC with no file of its own beside this tool's copy of its game gets one.
        if (!_settings.dlcInGameFile && !installsGameFiles() && hasBase && main.title->madeHere && !update.title) {
            for (auto& dlc : chosenDlcs) {
                if (withoutFile.count(dlc.title->id))
                    rows.back().dlcWithoutFile.push_back(dlc.title->id);
            }
        }
        // Made again or not, what this tool made only grows repacked with compression off.
        rows.back().compressedFile = compressed;
    }
    // An update file on its own, for a game that's installed and older: it's installed made part of what's installed, compressed with it.
    // That's whatever's installed, compressed or not, and the original game's file isn't needed.
    if (_console) {
        std::map<u64, IndexedCopy> newest;
        std::map<u64, std::vector<IndexedCopy>> all;
        for (auto& candidate : updates) {
            u64 application = candidate.title->linked;
            if (basesOf.count(application))
                continue;
            all[application].push_back(candidate);
            IndexedCopy& best = newest[application];
            if (!best.title || candidate.title->version > best.title->version)
                best = candidate;
        }
        for (auto& item : newest) {
            const Row* installed = nullptr;
            for (auto& row : _rows) {
                if (row.tab == TabInstalled && row.application == item.first)
                    installed = &row;
            }
            if (installed && installed->ready && installed->gameVersion < item.second.title->version) {
                Row row = rowForUpdate(item.second, *installed);
                for (auto& other : all[item.first]) {
                    const std::string& path = *other.path;
                    if (path != row.updateFile && std::find(row.otherUpdates.begin(), row.otherUpdates.end(), path) == row.otherUpdates.end()) {
                        row.otherUpdates.push_back(path);
                        // The update that's in a copy, put beside it, can be what makes it fit, so what was found out without it doesn't count.
                        row.fingerprint += format("+%u:%lld", other.title->version, (long long)other.title->bytes);
                    }
                }
                rows.push_back(std::move(row));
            }
        }
    }

    for (auto& row : rows) {
        if (!row.extracted.empty() && alsoAsFiles.count(row.application))
            row.notes = alsoAsFiles[row.application];
    }
    return rows;
}

App::Row App::rowForUpdate(const IndexedCopy& update, const Row& installed) {
    const IndexedTitle& title = *update.title;
    Row row;
    row.tab = TabFiles;
    row.key = *update.path + format("#%016llx", (unsigned long long)title.id);
    row.titleId = title.linked;
    row.updateFile = *update.path;
    row.overInstalled = true;
    // What's installed can be read in any order, and the update is read out of order.
    row.solidBytes = title.solidBytes;
    row.fingerprint = format("%lld+%s", (long long)title.bytes, installed.fingerprint.c_str());
    row.name = !title.name.empty() ? title.name : installed.name;
    row.detail = joined({format("已安装游戏的 v%u 更新补丁", title.version), whereIs(*update.path), fileNameOf(*update.path)}, Dot);
    row.tile = tileColor(row.name);
    // What the game would take up with the update installed beside it, as it normally would be.
    row.now = installed.now + title.bytes;
    row.application = title.linked;
    row.hasUpdate = true;
    row.gameVersion = title.version;
    row.version = !title.displayVersion.empty() ? title.displayVersion : installed.version;
    row.icon = title.icon ? title.icon : installed.icon;
    row.notes.push_back("这是已安装游戏的更新补丁。安装时会压缩并整合进游戏，替换现有内容，无需原始游戏文件。");
    return row;
}

/// Which of copies just as new to use, by where it is: the SD card, then a USB drive, then the game card, then a PC on the cable, then one
/// over the network, or the other way round with Wi-Fi first.
int App::placeRank(const std::string& path) {
    if (isCardPath(path))
        return 2;
    if (remote::isRemote(path))
        return startsWithNoCase(path, "pc:USB/") != remote::wifiFirst() ? 3 : 4;
    for (auto& drive : _drives) {
        if (startsWithNoCase(path, drive.root))
            return drive.isUsb ? 1 : 0;
    }
    return 0;
}

/// The newest of the copies, and of copies just as new, one that first says goes first, if it's given, then the nearest by placeRank, and then
/// one that isn't packed with NCZ, as that takes longer to read. The others are noted.
App::IndexedCopy App::newestCopy(std::vector<IndexedCopy> copies, const char* kind, std::vector<std::string>& notes, const std::function<bool(const IndexedCopy&)>& first) {
    auto isNcz = [](const IndexedCopy& c) { return endsWithNoCase(*c.path, ".nsz") || endsWithNoCase(*c.path, ".xcz"); };
    std::stable_sort(copies.begin(), copies.end(), [&](const IndexedCopy& a, const IndexedCopy& b) {
        if (a.title->version != b.title->version)
            return a.title->version > b.title->version;
        if (first && first(a) != first(b))
            return first(a);
        if (placeRank(*a.path) != placeRank(*b.path))
            return placeRank(*a.path) < placeRank(*b.path);
        return !isNcz(a) && isNcz(b);
    });
    for (size_t i = 1; i < copies.size(); i++) {
        std::string where = fileNameOf(*copies[i].path);
        notes.push_back(copies[i].title->version == copies[0].title->version ? format("此%s还有一份副本位于「%s」", kind, where.c_str())
                                                                             : format("较旧的%s（v%u）位于「%s」", kind, copies[i].title->version, where.c_str()));
    }
    return copies[0];
}

/// Which copy of a title, and of its update, goes on the list. What this tool already compressed takes the place of what it was made from
/// when it's as new, and then there's no update to go with it. With holdsAll given, of copies as new, one in a file that holds the game's DLC
/// goes first. A copy of the game that doesn't hold them gives way to its original, unless it's newer, when one of them has no file of its own.
/// True if what's chosen is compressed.
bool App::chooseCopy(const std::vector<IndexedCopy>& all, const std::vector<IndexedCopy>& updatesFor, const char* kind, std::vector<std::string>& notes, IndexedCopy& main, IndexedCopy& update,
                     const std::function<bool(const IndexedFile&)>& holdsAll, bool dlcWithoutFile) {
    auto holds = [&](const IndexedCopy& copy) { return holdsAll && holdsAll(*copy.file); };
    // A DLC with nothing to compress goes into a file as it is, so its headers don't say this tool made it. In a file with a game this tool
    // made, or with all its game's DLC and something else this tool made, this tool put it there, as small as it gets, and it counts as
    // compressed with them.
    auto madeHere = [&](const IndexedCopy& copy) {
        if (copy.title->madeHere)
            return true;
        if (copy.title->type != Cnmt::MetaAddOnContent)
            return false;
        bool held = holds(copy);
        auto& titles = copy.file->titles;
        return std::any_of(titles.begin(), titles.end(), [&](const IndexedTitle& title) {
            return title.madeHere && (held || (title.type == Cnmt::MetaApplication && title.id == copy.title->linked));
        });
    };
    std::vector<IndexedCopy> originals, compressed;
    for (auto& copy : all)
        (madeHere(copy) ? compressed : originals).push_back(copy);
    update = IndexedCopy();
    u32 madeFrom = 0;
    if (!originals.empty()) {
        main = newestCopy(originals, kind, notes, holdsAll ? holds : std::function<bool(const IndexedCopy&)>());
        if (!updatesFor.empty()) {
            IndexedCopy candidate = newestCopy(updatesFor, "update", notes);
            bool goesWith = candidate.title->version > main.title->version || (candidate.title->version == main.title->version && main.title->holdsOnlyPart);
            if (goesWith)
                update = candidate;
            else
                notes.push_back(format("v%u 更新补丁（位于「%s」）已略过，因为本体已是 v%u", candidate.title->version, fileNameOf(*candidate.path).c_str(), main.title->version));
        }
        madeFrom = std::max(main.title->version, update.title ? update.title->version : 0u);
    }
    std::stable_sort(compressed.begin(), compressed.end(), [&](const IndexedCopy& a, const IndexedCopy& b) {
        if (a.title->version != b.title->version)
            return a.title->version > b.title->version;
        if (holds(a) != holds(b))
            return holds(a);
        return placeRank(*a.path) < placeRank(*b.path);
    });
    // A copy without a DLC that has no file of its own gives way to its original, which can put it in the new file. One whose DLC all have a
    // file of their own, as 1.5.0 left them, is kept and made again with them.
    bool givesWay = holdsAll && dlcWithoutFile && !compressed.empty() && compressed[0].title->type == Cnmt::MetaApplication && !holds(compressed[0]);
    if (!compressed.empty() && (originals.empty() || compressed[0].title->version > madeFrom || (compressed[0].title->version == madeFrom && !givesWay))) {
        for (size_t i = 1; i < compressed.size(); i++)
            notes.push_back(format("另一份压缩副本（v%u）位于「%s」", compressed[i].title->version, fileNameOf(*compressed[i].path).c_str()));
        if (!originals.empty()) {
            std::string from = "'" + fileNameOf(*main.path) + "'" + (update.title ? " 和 '" + fileNameOf(*update.path) + "'" : std::string());
            notes.insert(notes.begin(), "压缩自 " + from + (update.title ? "，也位于此处" : "，也在此处"));
        }
        main = compressed[0];
        update = IndexedCopy();
        return true;
    }
    for (auto& older : compressed) {
        notes.push_back(older.title->version < madeFrom
                            ? format("较旧的压缩副本（v%u）位于「%s」；再次压缩会生成 v%u 版本", older.title->version, fileNameOf(*older.path).c_str(), madeFrom)
                            : format("缺少部分 DLC 的已压缩副本位于「%s」。再次压缩会生成包含全部 DLC 的副本", fileNameOf(*older.path).c_str()));
    }
    return false;
}

App::Row App::rowForExtracted(const std::string& path, const IndexedTitle& title, bool done) {
    Row row;
    row.tab = TabFiles;
    row.key = path;
    row.fingerprint = format("%lld", (long long)title.bytes);
    row.name = title.name;
    row.detail = joined({title.version ? format("v%u", title.version) : "", whereIs(path), fileNameOf(path)}, Dot);
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

App::Row App::rowForGame(const IndexedCopy& main, const IndexedCopy* update, const std::vector<IndexedCopy>& dlcs, std::vector<std::string> notes, bool compressed, int apart) {
    const IndexedTitle& title = *main.title;
    Row row;
    row.tab = TabFiles;
    row.key = *main.path + format("#%016llx", (unsigned long long)title.id);
    row.titleId = title.id;
    row.mainFile = *main.path;
    i64 bytes = title.bytes;
    if (update) {
        row.updateFile = *update->path;
        // Made one with its update, the game is read out of order, and so is the update itself.
        row.solidBytes = title.solidBytes + update->title->solidBytes;
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
    dlcName += " (DLC)";
    row.name = update && !update->title->name.empty() ? update->title->name : !title.name.empty() ? title.name : isBase ? fileName : dlcName;
    std::string what = !isBase ? format("DLC v%u", title.version)
                     : compressed || title.madeHere ? format("已压缩，v%u", title.version)
                     : update ? format("本体 v%u ＋ 更新 v%u", title.version, update->title->version)
                              : format("base v%u", title.version);
    // A row of DLC alone already says it's DLC.
    int dlcCount = (int)dlcs.size() + (isBase ? 0 : 1);
    bool sayDlc = isBase ? dlcCount > 0 : dlcCount > 1;
    std::string dlcSaid = !sayDlc ? std::string() : apart == 0 ? format("%d DLC", dlcCount) : apart == dlcCount ? format("%d 个 DLC 未并入文件", dlcCount)
                                                                                   : format("%d 个 DLC，其中 %d 个未并入文件", dlcCount, apart);
    row.detail = joined({what, dlcSaid, whereIs(*main.path), isCardPath(*main.path) ? std::string() : fileNameOf(*main.path)}, Dot);
    row.tile = tileColor(row.name);
    row.now = bytes;
    row.application = isBase ? title.id : title.linked;
    row.tiny = isBase && !update && bytes < SmallGameBytes;
    row.hasUpdate = update != nullptr;
    row.gameVersion = update ? std::max(title.version, update->title->version) : title.version;
    row.version = update && !update->title->displayVersion.empty() ? update->title->displayVersion : title.displayVersion;
    row.icon = update && update->title->icon ? update->title->icon : title.icon;
    if (title.holdsOnlyPart && !update)
        row.blocked = "这只包含游戏的一部分，其余内容在更新补丁里";
    // What this tool made, as new as what it was made from, is compressed, which its headers said when it was first come across. All that's
    // left is installing it, if game files get installed. A copy made since 1.5 says what the game came to before, so what's smaller about
    // it. With compression off, it's analyzed like anything else, to say how big it gets.
    row.compressedFile = compressed;
    if (compressed && _settings.compress) {
        row.hasEstimate = true;
        row.estimate.inputBytes = row.estimate.outputBytes = bytes;
        row.estimate.alreadyCompressed = true;
        i64 freed = title.originalBytes > 0 ? title.originalBytes - title.bytes : 0;
        for (auto& dlc : dlcs)
            freed += dlc.title->originalBytes > 0 ? dlc.title->originalBytes - dlc.title->bytes : 0;
        row.estimate.freedBytes = std::max<i64>(freed, 0);
        row.ticked = installsGameFiles();
    }
    return row;
}

// -------------------------------------------------------------------------------------------------------------------
// Game files: opening them, once there's something to do with them

/// Opened only for as long as that takes. A file that's open can't be deleted or written over.
App::OpenedJob App::openJob(const Row& row) {
    if (row.overInstalled)
        return openJobOverInstalled(row);
    std::string files = row.mainFile + "|" + row.updateFile;
    for (auto& dlc : row.dlcFiles)
        files += "|" + dlc;
    if (!_openSet || _openSetFiles != files) {
        _openSet.reset();
        auto set = std::make_shared<TitleSet>(_keyset);
        std::set<std::string> added;
        for (auto* path : {&row.mainFile, &row.updateFile}) {
            if (!path->empty() && added.insert(*path).second)
                set->add(archiveFor(*path));
        }
        for (auto& dlc : row.dlcFiles) {
            if (added.insert(dlc).second)
                set->add(archiveFor(dlc));
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

/// An update file with the installed game as its base, whether that's the original or a copy this compressed, with the parts of the
/// original its earlier update kept, which the map in its control data says where to find.
App::OpenedJob App::openJobOverInstalled(const Row& row) {
    std::string files = format("installed:%016llx|", (unsigned long long)row.application) + row.updateFile;
    for (auto& other : row.otherUpdates)
        files += "|" + other;
    if (!_openSet || _openSetFiles != files) {
        _openSet.reset();
        if (!_console)
            throw ToolError("没有可读取已安装游戏的主机");
        InstalledGame game = _console->describe(row.application);
        // An update installed over it already is what this file's update takes over from, as from any older update.
        judgeUpdateOver(game);
        if (!game.blocked.empty())
            throw ToolError(game.blocked);
        auto set = std::make_shared<TitleSet>(_keyset);
        _console->addTitleKeys(set->tickets(), _keyset);
        set->add(std::make_shared<InstalledArchive>(*_console, game));
        set->add(std::make_shared<FileArchive>(row.updateFile));
        for (auto& other : row.otherUpdates)
            set->add(std::make_shared<FileArchive>(other));
        set->group(true);
        _openSet = set;
        _openSetFiles = files;
    }
    for (auto& job : _openSet->jobs) {
        if (job.main->meta.titleId == row.titleId && job.update && job.update->archive->path() == row.updateFile)
            return OpenedJob{_openSet, &job};
    }
    std::string why = !_openSet->problems.empty() ? _openSet->problems[0].message : std::string("此更新补丁与已安装游戏不匹配");
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
    row.underMinimum = false;
    row.notSure = false;
    // A packed file, such as an NSZ, is compared with what it takes installed as it is when it's installed, as every other installer unpacks
    // it, and with the file itself when an NSP is made of it.
    if (row.hasEstimate && row.estimate.packedBytes > 0 && row.estimate.installedBytes > 0)
        row.estimate.inputBytes = goesOnConsole(row) ? row.estimate.installedBytes : row.estimate.packedBytes;
    if (row.updateOverCopy) {
        // It won't start as it is, which matters more than whatever making its update part of it saves.
    } else if (row.hasEstimate && options.compress) {
        // A compressed game file can still be installed, which is all that's left to do with it.
        // One its developer compressed (byMakers) is judged as any other on what making it again here saves, and is to do when that's worth
        // it, but it's never ticked by itself, below: its updates then need its original, which its line and the question say.
        if (!row.dlcWithoutFile.empty()) {
            // A DLC with no file of its own gets one, whatever that saves.
        } else if (row.estimate.alreadyCompressed) {
            // Made again only to put its DLC in its file, it saves nothing, and is judged as any game that saves nothing.
            if (row.dlcNotInFile)
                row.belowMinimum = "没有收益：已经是最小的了";
            else if (!compressedToInstallLocked(row))
                row.belowMinimum = "已压缩";
        } else if (SavingRule::notSure(row.estimate, options)) {
            // A first look can say a game is worth compressing, but not that it isn't. Measure reads it to know, and until then it isn't ticked.
            row.notSure = true;
        } else if (row.estimate.savedFraction() < -GrowsForSure) {
            // Only growing that's a fact gets here (sureToGrow): over its size on a first look alone, a game is not sure, above.
            row.belowMinimum = format("无收益：体积会增大 %.0f%%", -row.estimate.savedFraction() * 100);
        } else if (row.estimate.sureSavedBytes() <= 0) {
            row.belowMinimum = "没有收益：已经是最小的了";
        } else if (!SavingRule::worthCompressing(row.estimate, options)) {
            // What the rule says goes without "it" on a PC, where nothing comes before it.
            row.belowMinimum = std::string(row.estimate.atLeast() && !row.estimate.measured ? "不值得：它是 " : "不值得：它 ") + SavingRule::reason(row.estimate, options);
            row.underMinimum = true;
        }
    } else if (!options.compress) {
        if (row.tab == TabFiles && installsGameFiles()) {
            // Installing passes what's compressed through as it is, so nothing grows: what counts is whether it's installed already.
            if (installedAsNewLocked(row))
                row.belowMinimum = "已安装";
        } else if (row.hasEstimate && row.estimate.savedFraction() < -NothingToGain) {
            // Repacked with compression off, a compressed game comes out uncompressed.
            row.belowMinimum = format("无收益：压缩已关闭，体积会增大 %.0f%%", -row.estimate.savedFraction() * 100);
        } else if (!row.hasEstimate && row.compressedFile) {
            row.belowMinimum = "无收益：已压缩，关闭压缩后体积反而增大";
        } else if (repackGainsNothingLocked(row)) {
            row.belowMinimum = "无可并入的更新补丁";
        }
    }
    if (!row.byHand)
        row.ticked = row.belowMinimum.empty() && !row.notSure && row.blocked.empty() && row.state != RowState::Done && row.state != RowState::Failed &&
                     !(row.hasEstimate && row.estimate.byMakers && !row.estimate.alreadyCompressed);
}

/// Whether a game file is done, and so ticked, depends on how its game is installed, which can change after it's been judged: once the
/// installed game has been analyzed, compressed, or replaced.
void App::judgeFilesOfLocked(u64 application) {
    ConvertOptions options = convertOptions();
    for (auto& row : _rows) {
        if (row.tab == TabFiles && row.application == application)
            judgeLocked(row, options);
    }
}

/// A game that's gone from the console, uninstalled from here or from anywhere else, takes with it what its game files were judged on: one
/// installed from here since the app opened is no longer done, and whether each is ticked is worked out again. They used to stay as installed
/// until the list was looked for again.
void App::installedGoneLocked(const std::set<u64>& applications) {
    ConvertOptions options = convertOptions();
    for (auto& row : _rows) {
        if (row.tab != TabFiles || !applications.count(row.application))
            continue;
        if (row.state == RowState::Done && installsGameFiles())
            row.state = RowState::Ready;
        judgeLocked(row, options);
    }
    _dirty = true;
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
    int seen[TabCount] = {};
    for (auto& row : _rows) {
        if (leftOffListLocked(row))
            continue;
        if (seen[row.tab]++ == _focus[row.tab] && _focus[row.tab] > 0)
            focused[row.tab] = row.key;
    }

    // A game being worked out again keeps its place until it has a new answer (Analyze again), and only until then.
    for (auto& row : _rows) {
        if (row.hasEstimate)
            row.kept = Row::Kept{};
    }
    auto kept = [](const Row& row) { return !row.hasEstimate && row.kept.rank >= 0; };

    int sort = _settings.sort;
    std::stable_sort(_rows.begin(), _rows.end(), [&](const Row& a, const Row& b) {
        if (a.tab != b.tab)
            return a.tab < b.tab;
        // What's been done is out of the way of what's still to do.
        bool aDone = kept(a) ? a.kept.done : isCompressed(a), bDone = kept(b) ? b.kept.done : isCompressed(b);
        if (aDone != bDone)
            return bDone;
        if (sort == 0 && _sortedByGain[a.tab]) {
            // What gains the most for the time it takes, as on the other versions (gainRank).
            auto rank = [&](const Row& row) { return kept(row) ? row.kept.rank : gainRank(row); };
            if (rank(a) != rank(b))
                return rank(a) < rank(b);
            // By what each is sure to save, as the list shows it.
            auto fraction = [&](const Row& row) { return kept(row) ? row.kept.fraction : row.estimate.sureSavedFraction(); };
            if (rank(a) < 3 && fraction(a) != fraction(b))
                return fraction(a) > fraction(b);
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
    return row.state == RowState::Done || (row.state == RowState::Ready && row.hasEstimate && row.estimate.alreadyCompressed && _settings.compress && !row.dlcNotInFile &&
                                           row.dlcWithoutFile.empty());
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
        if (installed.tab == TabInstalled && installed.application == row.application && installed.ready && installed.storage != Storage::GameCard)
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
    if (!_settings.compress)
        return repackGainsNothingLocked(row);
    if (row.tab == TabFiles && installsGameFiles())
        return installedCompressedLocked(row);
    // Settled, saying why: measured not worth it, nothing to gain, or compressed already, by this version, an older one or its developer, and
    // not worth compressing again. To do keeps what's worth doing and what isn't known yet. A copy an older version made is done as it was,
    // saying what it freed, until compressing it again is sure to be worth it, from its first look or once it's measured: then it's to do,
    // and ticked.
    if (row.state == RowState::Ready && row.hasEstimate && !row.belowMinimum.empty() && !row.notSure)
        return true;
    if (row.state == RowState::Ready && row.hasEstimate && row.estimate.olderCopy && row.notSure)
        return true;
    return isCompressed(row);
}

/// A compressed game file that's to be installed, and isn't yet, which only takes copying it across.
bool App::compressedToInstallLocked(const Row& row) const {
    return row.tab == TabFiles && installsGameFiles() && isCompressed(row) && !isDoneLocked(row);
}

/// With compression off, a repack only gains anything by making a game's update part of it. One in one piece already, whether it was never
/// updated, was merged by another tool or was repacked here, has nothing left to do, and nothing marks it on the console but that, so it's
/// the same whenever the app opens. An extracted game still has an NSP to be made of it. Game files that get installed are done once their game
/// is installed, as new or newer.
bool App::repackGainsNothingLocked(const Row& row) const {
    if (row.tab == TabFiles && installsGameFiles())
        return installedAsNewLocked(row);
    if (!row.extracted.empty())
        return false;
    return row.tab == TabInstalled ? !row.hasUpdate : row.updateFile.empty();
}

/// A game file whose game is installed as new or newer, in whatever form. What the console has recorded as the version can be changed, as
/// DBI's reset of the required version does, so the version the game itself gives being the same counts too.
bool App::installedAsNewLocked(const Row& row) const {
    if (row.tab != TabFiles || !_console)
        return false;
    // DLC on its own is for a game, not the game itself.
    if (row.extracted.empty() && row.titleId != row.application)
        return false;
    for (auto& installed : _rows) {
        if (installed.tab == TabInstalled && installed.application == row.application && installed.ready && installed.storage != Storage::GameCard)
            return installed.gameVersion >= row.gameVersion || (!row.version.empty() && row.version == installed.version);
    }
    return false;
}

/// Done is what there's nothing left to do with: compressed, or with compression off, nothing for a repack to gain (repackGainsNothingLocked).
/// Games under 1 MB, when they're hidden, are left out of every view, as they always were: they're forwarders, which nobody wants listed.
bool App::inViewLocked(const Row& row, View view) const {
    if (_settings.hideSmall && row.tiny)
        return false;
    switch (view) {
        case View::Everything:
            return true;
        case View::Done:
            return isDoneLocked(row);
        default:
            return !isDoneLocked(row);
    }
}

bool App::leftOffListLocked(const Row& row) const { return !inViewLocked(row, _view[row.tab]); }

/// What's done and not on the list as it's shown, so that a list with nothing left to do can say where everything went.
int App::doneHiddenLocked(int tab) const {
    int hidden = 0;
    for (auto& row : _rows)
        hidden += row.tab == tab && inViewLocked(row, View::Done) && leftOffListLocked(row) ? 1 : 0;
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

/// Every game on the list without an answer, ticked or not, whichever view is shown: every game gets one by itself, and one left unticked
/// can be ticked later. A game with no answer isn't under Done, so one asked to be worked out again from there would otherwise wait until
/// To do was gone to. Games under 1 MB, while they're hidden, aren't in any view.
std::vector<std::string> App::keysToAnalyzeLocked(int tab) {
    std::vector<std::string> keys;
    for (auto& row : _rows) {
        if (row.tab == tab && !(_settings.hideSmall && row.tiny) && row.ready && !row.hasEstimate && row.blocked.empty() && row.state == RowState::Ready)
            keys.push_back(row.key);
    }
    return keys;
}

}  // namespace hbp
