// The work: analyzing what each game would gain, and compressing, installing, extracting, checking and removing games, with how far
// along it all is.
#include "app_internal.h"

#include <cmath>
#include <cstdio>

#include "../core/remote.h"

namespace hbp {

using namespace app;

namespace {

constexpr const char* StepCompress = "压缩";
/// Installing what's already compressed only copies it across, without compressing any of it again.
constexpr const char* StepCopy = "复制过去";
constexpr const char* StepPrepare = "准备就绪";
constexpr const char* StepWrite = "写入";
constexpr const char* StepCheck = "逐字节校验";
constexpr const char* StepHash = "计算哈希";
constexpr const char* StepCheckNsp = "校验写入内容";
constexpr const char* StepFirmware = "回读";
constexpr const char* StepSwap = "替换";

const char* const NotEnoughMemory = "内存不足，无法处理此项";

}  // namespace

struct App::LoadedGame {
    InstalledGame game;
    std::shared_ptr<InstalledArchive> archive;
    std::unique_ptr<TitleSet> set;
};

std::unique_ptr<App::LoadedGame> App::loadGame(u64 application) {
    auto loaded = std::make_unique<LoadedGame>();
    loaded->game = _console->describe(application);
    {
        // What it's called goes into the names of files made from it, and is already known.
        sys::Lock lock(_mutex);
        if (Row* row = findLocked(format("%016llx", (unsigned long long)application)))
            loaded->game.name = row->name;
    }
    loaded->archive = std::make_shared<InstalledArchive>(*_console, loaded->game);
    loaded->set.reset(new TitleSet(_keyset));
    _console->addTitleKeys(loaded->set->tickets(), _keyset);
    loaded->set->add(loaded->archive);
    loaded->set->group();
    for (auto& problem : loaded->set->problems)
        logLine(problem.message);
    if (loaded->set->jobs.empty())
        throw ToolError(loaded->set->problems.empty() ? std::string("此游戏中没有可重建的内容") : loaded->set->problems[0].message);

    // DLC that can't be read can stay as it is, but there's no saying anything about a game without the game itself.
    bool hasBase = std::any_of(loaded->game.metas.begin(), loaded->game.metas.end(), [](const InstalledMeta& meta) { return meta.key.type == Cnmt::MetaApplication; });
    bool baseLoaded = std::any_of(loaded->set->jobs.begin(), loaded->set->jobs.end(), [](const TitleJob& job) { return job.isBaseGame(); });
    if (hasBase && !baseLoaded)
        throw ToolError(loaded->set->problems.empty() ? std::string("无法读取其本体游戏") : loaded->set->problems[0].message);
    return loaded;
}

// -------------------------------------------------------------------------------------------------------------------
// Analyzing

/// There's no working out what a game would gain if all of it is already stored the way this would store it.
SizeEstimate App::estimateJob(const TitleJob& job, const ConvertOptions& options) {
    if (isAlreadyCompressed(job, options)) {
        SizeEstimate same;
        same.inputBytes = same.outputBytes = job.inputBytes;
        same.alreadyCompressed = true;
        return same;
    }
    return estimateTitle(job, options);
}

/// What the PC sharing a game says it comes to, worked out the same way, with these settings. False if it can't say, as an older version
/// can't, and it's to be worked out here.
bool App::estimateOnPc(const Row& row, SizeEstimate& estimate) {
    std::vector<std::string> paths;
    if (!row.extracted.empty()) {
        paths.push_back(row.extracted);
    } else {
        paths.push_back(row.mainFile);
        if (!row.updateFile.empty() && row.updateFile != row.mainFile)
            paths.push_back(row.updateFile);
        for (auto& dlc : row.dlcFiles) {
            if (std::find(paths.begin(), paths.end(), dlc) == paths.end())
                paths.push_back(dlc);
        }
    }
    std::string settings = format("%d\t%d\t%d\t%016llx", _settings.level, _settings.blockSize, _settings.compress ? 1 : 0, (unsigned long long)row.application);
    std::string answer;
    try {
        if (!remote::estimate(paths, settings, answer, [] { checkStop(); }))
            return false;
    } catch (const ToolError&) {
        return false;
    }
    long long in = 0, out = 0, margin = 0, romFs = 0, stored = 0;
    int compressed = 0;
    if (sscanf(answer.c_str(), "%lld %lld %lld %lld %lld %d", &in, &out, &margin, &romFs, &stored, &compressed) != 6)
        return false;
    estimate = SizeEstimate();
    estimate.inputBytes = row.now;
    // Whatever the PC didn't go through stays just as big as it is, the same as here.
    estimate.outputBytes = out + std::max<i64>(row.now - in, 0);
    estimate.marginBytes = margin;
    estimate.romFsBytes = romFs;
    estimate.romFsStoredBytes = stored;
    estimate.alreadyCompressed = compressed != 0;
    logLine(format("Analyzed on the PC: %s", row.name.c_str()));
    return true;
}

SizeEstimate App::estimateRow(const Row& row) {
    ConvertOptions options = convertOptions();
    std::string where = !row.extracted.empty() ? row.extracted : row.mainFile;
    // A PC sharing the game works it out far faster than reading samples of it across the network would.
    SizeEstimate fromPc;
    if (row.tab != TabInstalled && remote::isRemote(where) && estimateOnPc(row, fromPc))
        return fromPc;
    // Only a USB drive can be a hard drive. A large game on the SD card can be slow to read as well, but that's not from going looking.
    auto drives = _platform.drives();
    options.noSeekTime = std::none_of(drives.begin(), drives.end(), [&](const Drive& drive) { return drive.isUsb && startsWithNoCase(where, drive.root); });
    if (!row.extracted.empty())
        return estimateBuild(openExtracted(row.extracted), options);
    if (row.tab != TabInstalled && row.dlcFiles.empty())
        return estimateJob(*openJob(row).job, options);

    // A base game with its update and each DLC come to one row.
    std::vector<const TitleJob*> jobs;
    std::shared_ptr<void> keep;
    if (row.tab != TabInstalled) {
        OpenedJobs opened = openJobs(row);
        jobs = opened.jobs;
        keep = opened.set;
    } else {
        auto loaded = loadGame(row.application);
        for (auto& job : loaded->set->jobs)
            jobs.push_back(&job);
        keep = std::shared_ptr<LoadedGame>(std::move(loaded));
    }
    SizeEstimate total;
    double margin = 0;
    i64 covered = 0;
    // DLC with no game data to it says nothing either way.
    bool anyCompressed = false, anyNot = false;
    for (const TitleJob* job : jobs) {
        SizeEstimate one = estimateJob(*job, options);
        anyCompressed = anyCompressed || one.alreadyCompressed;
        // A part with nothing to gain, such as DLC with a few hundred bytes of game data, can't be told apart from compressed.
        anyNot = anyNot || (!one.alreadyCompressed && one.romFsBytes > 0 && one.savedFraction() >= NothingToGain);
        total.outputBytes += one.outputBytes;
        total.romFsBytes += one.romFsBytes;
        total.romFsStoredBytes += one.romFsStoredBytes;
        margin += (double)one.marginBytes * (double)one.marginBytes;
        covered += job->inputBytes;
    }
    total.marginBytes = (i64)sqrt(margin);
    total.alreadyCompressed = anyCompressed && !anyNot;
    total.inputBytes = row.now;
    // Whatever couldn't be gone through stays just as big as it is.
    total.outputBytes += std::max<i64>(row.now - covered, 0);
    return total;
}

void App::analyze(const std::vector<std::string>& keys) {
    {
        sys::Lock lock(_mutex);
        _analysisCount = (int)keys.size();
        _analysisIndex = 0;
    }
    int tab = -1;
    struct Sorted {
        App& app;
        int& tab;
        // However it ended, what's known by now goes in order.
        ~Sorted() {
            sys::Lock lock(app._mutex);
            if (tab >= 0)
                app._sortedByGain[tab] = true;
            app.sortRowsLocked();
            if (app._cacheChanged)
                app.saveCacheLocked();
            app.closeOpenJob();
            app._dirty = true;
        }
    } sorted{*this, tab};

    for (auto& key : keys) {
        checkStop();
        Row copy;
        {
            sys::Lock lock(_mutex);
            _analysisIndex++;
            Row* found = findLocked(key);
            if (!found)
                continue;
            found->state = RowState::Working;
            copy = *found;
            tab = copy.tab;
            _analysisTitle = copy.name;
            _dirty = true;
        }

        std::string failure;
        bool mayWorkNextTime = false;
        SizeEstimate estimate;
        double analyzeStarted = sys::now();
        try {
            logLine("Analyzing " + copy.name);
            estimate = estimateRow(copy);
            logLine(format("%s took %.2f s", copy.name.c_str(), sys::now() - analyzeStarted));
        } catch (const ToolError& e) {
            failure = e.what();
            // A drive that's been taken out can come back, where a game with no key for it will still have none.
            mayWorkNextTime = failure.find("Couldn't read") != std::string::npos || failure.find("being looked through") != std::string::npos;
            logLine(copy.name + ": " + failure);
        } catch (const std::bad_alloc&) {
            failure = NotEnoughMemory;
            mayWorkNextTime = true;
            logLine(copy.name + ": " + failure);
        } catch (const Cancelled&) {
            sys::Lock lock(_mutex);
            if (Row* found = findLocked(key))
                found->state = RowState::Ready;
            throw;
        }

        sys::Lock lock(_mutex);
        Row* found = findLocked(key);
        if (!found)
            continue;
        found->state = failure.empty() ? RowState::Ready : RowState::Failed;
        found->message = failure;
        if (failure.empty()) {
            // If this one's about to go from the list, the focus stays on whatever it was on.
            int above = 0;
            for (auto* row : visibleRowsLocked(_tab)) {
                if (row == found)
                    break;
                above++;
            }
            bool wasShown = !leftOffListLocked(*found);
            found->hasEstimate = true;
            found->estimate = estimate;
            // The options may have been changed by now.
            judgeLocked(*found, convertOptions());
            rememberEstimateLocked(*found);
            if (wasShown && leftOffListLocked(*found) && found->tab == _tab && _tab < TabCount && above < _focus[_tab])
                _focus[_tab]--;
        } else {
            if (!found->byHand)
                found->ticked = false;
            if (!mayWorkNextTime)
                rememberFailureLocked(*found, failure);
        }
        _dirty = true;
    }
}

// -------------------------------------------------------------------------------------------------------------------
// Making room

/// Refuses early rather than run out of room halfway. The new copy goes beside the old one, wherever that is.
void App::needRoom(Storage storage, i64 output) {
    i64 needed = output + RoomToSpare;
    i64 free = _console ? _console->freeBytes(storage) : sys::freeSpace(_platform.dataFolder());
    if (free < needed)
        throw ToolError(format("处理过程中需要 %s 上的 %s 可用空间，目前只有 %s。先压缩较小的游戏可腾出空间", storageName(storage), formatSize(needed).c_str(), formatSize(free).c_str()));
}

void App::needRoomIn(const std::string& folder, i64 output) {
    sys::makeDirectories(folder);
    if (!sys::isDirectory(folder))
        throw ToolError("文件夹" + folder + "无法访问。如果在 USB 可移动设备上，请确认已插好。");
    if (output >= sys::largestFile(folder))
        throw ToolError(format("输出大小为 %s，%s 放不下这么大的文件。FAT32 单文件上限为 4 GB，需改用 exFAT 或 NTFS", formatSize(output).c_str(), folder.c_str()));
    i64 needed = output + RoomToSpare;
    i64 free = sys::freeSpace(folder);
    if (free < needed)
        throw ToolError(format("需要 %s 可用空间于 %s，目前只有 %s", formatSize(needed).c_str(), folder.c_str(), formatSize(free).c_str()));
}

bool App::goesOnConsole(const Row& row) const {
    if (row.tab == TabInstalled)
        return replacesInstalled();
    return _console && (_installNow || _settings.installFiles);
}

/// Goes through what's ticked to see what fits, in the order given unless what one game frees is what makes room for another.
App::RoomPlan App::planRoom() {
    struct Item {
        std::string key;
        std::string name;
        std::string pool;
        i64 peak = 0;
        i64 change = 0;
        std::string tooLarge;
    };

    std::string folder = nspFolder();
    auto poolOf = [&](const std::string& path) -> std::string {
        for (auto& drive : _drives) {
            if (drive.isUsb && startsWithNoCase(path, drive.root))
                return drive.root;
        }
        return "SD";
    };

    std::vector<Item> items;
    {
        sys::Lock lock(_mutex);
        for (auto* row : visibleRowsLocked(_tab)) {
            if (!row->ticked || !row->blocked.empty() || !row->ready)
                continue;
            Item item;
            item.key = row->key;
            item.name = row->name;
            // Compressed, what's made can't come to much more than what it's made from, and not compressed, it's already been worked out.
            i64 output = row->hasEstimate ? row->estimate.outputBytes + row->estimate.marginBytes : row->now + row->now / 8;
            item.peak = output + RoomToSpare;
            if (goesOnConsole(*row)) {
                bool toConsole = row->tab == TabInstalled ? row->storage == Storage::Nand : _settings.installToConsole;
                item.pool = toConsole ? "主机内存" : "SD";
                item.change = (row->tab == TabInstalled ? row->now : 0) - output;
                if (row->tab != TabInstalled && row->extracted.empty() && _settings.deleteInstalled && !toConsole && poolOf(row->mainFile) == "SD")
                    item.change += row->now;
            } else {
                item.pool = poolOf(folder);
                item.change = -output;
                if (output >= sys::largestFile(folder))
                    item.tooLarge = format("%s 放不下 %s 的文件", folder.c_str(), formatSize(output).c_str());
            }
            items.push_back(std::move(item));
        }
    }

    std::map<std::string, i64> free;
    auto freeIn = [&](const std::string& pool) -> i64& {
        auto found = free.find(pool);
        if (found != free.end())
            return found->second;
        i64 bytes;
        if (pool == "主机内存")
            bytes = _console ? _console->freeBytes(Storage::Nand) : 0;
        else if (pool == "SD")
            bytes = _console ? _console->freeBytes(Storage::Sd) : sys::freeSpace(folder);
        else
            bytes = sys::freeSpace(pool);
        return free[pool] = bytes;
    };

    RoomPlan plan;
    std::vector<bool> done(items.size(), false);
    for (bool progress = true; progress;) {
        progress = false;
        for (size_t i = 0; i < items.size(); i++) {
            if (done[i] || !items[i].tooLarge.empty())
                continue;
            i64& room = freeIn(items[i].pool);
            if (items[i].peak <= room) {
                room += items[i].change;
                plan.order.push_back(items[i].key);
                done[i] = true;
                progress = true;
            }
        }
    }
    for (size_t i = 0; i < items.size(); i++) {
        if (done[i])
            continue;
        plan.wontFit.push_back(items[i].name);
        plan.mostNeeded = std::max(plan.mostNeeded, items[i].peak - freeIn(items[i].pool));
        if (!items[i].tooLarge.empty())
            plan.reason = items[i].tooLarge;
        else if (plan.reason.empty())
            plan.reason = items[i].pool == "主机内存" ? "主机" : items[i].pool == "SD" ? "SD 卡" : "USB 可移动设备";
    }
    return plan;
}

// -------------------------------------------------------------------------------------------------------------------
// How far along it is

std::vector<const char*> App::stepsFor(const Row& row) const {
    bool check = _settings.checkResult;
    if (goesOnConsole(row)) {
        if (_settings.compress) {
            // What this tool made says so in its name, before anything's been read of it.
            bool allMade = row.updateFile.empty() && isOwnOutput(row.mainFile) && std::all_of(row.dlcFiles.begin(), row.dlcFiles.end(), isOwnOutput);
            const char* first = row.tab != TabInstalled && (isCompressed(row) || allMade) ? StepCopy : StepCompress;
            return check ? std::vector<const char*>{first, StepCheck, StepFirmware, StepSwap} : std::vector<const char*>{first, StepHash, StepSwap};
        }
        return check ? std::vector<const char*>{StepPrepare, StepWrite, StepCheck, StepFirmware, StepSwap} : std::vector<const char*>{StepPrepare, StepWrite, StepSwap};
    }
    if (_settings.compress)
        return {StepCompress, check ? StepCheckNsp : StepHash};
    return {StepPrepare, StepWrite};
}

/// What a game comes to in what has to be got through. The sizes of what's on the disk don't say much about that: a game with an update takes up
/// a good deal more than what gets compressed in the end, and what's kept in memory never has to be read back.
void App::workPlannedFor(const Row& row, JobKind kind, i64 (&planned)[2]) const {
    planned[0] = row.now;
    planned[1] = 0;
    if (kind != JobKind::Compressing)
        return;
    if (!_settings.compress) {
        // Once to check it and work out its hash tree, and once to write it.
        planned[0] = row.now * 2;
    } else if (row.hasEstimate && row.estimate.romFsBytes > 0) {
        planned[0] = row.estimate.romFsBytes;
    }
    // A new copy of a game on the console gets all of it read back if it's being checked, and otherwise, whatever fits in memory doesn't.
    i64 output = row.hasEstimate ? row.estimate.outputBytes : row.now;
    planned[1] = _settings.checkResult && goesOnConsole(row) ? output : std::max<i64>(output - sys::spareMemory(), 0);
}

void App::prepareJob(const std::vector<std::string>& keys, JobKind kind) {
    sys::Lock lock(_mutex);
    _results.clear();
    _jobCount = (int)keys.size();
    _jobIndex = 0;
    _jobFreed = 0;
    _jobSteps.clear();
    _jobStep = -1;
    _work[0] = _work[1] = Work();
    _gamePlanned[0] = _gamePlanned[1] = 0;
    _gameDoneBefore[0] = _gameDoneBefore[1] = 0;
    _gamesFinished = 0;
    _workKindNow = -1;
    _workLabel.clear();
    _workLastDone = 0;
    _workLastAt = sys::now();
    _overallShown = 0;
    for (auto& key : keys) {
        if (Row* found = findLocked(key)) {
            i64 planned[2];
            workPlannedFor(*found, kind, planned);
            _work[0].planned += planned[0];
            _work[1].planned += planned[1];
        }
    }
    if (Row* first = findLocked(keys[0])) {
        _jobTitle = first->name;
        _jobIcon = iconForJob(*first);
        _jobTile = first->tile;
    }
}

/// The large one if it's there already, and otherwise the one from the list, which is what the game looks like until the large one's been read.
std::shared_ptr<Image> App::iconForJob(const Row& row) {
    if (row.bigIcon)
        return row.bigIcon;
    return row.icon ? std::make_shared<Image>(row.icon->scaledTo(JobIconSize, JobIconSize)) : nullptr;
}

/// The large icon for the job screen, which only gets read when it's needed, as most of the list never goes through here.
void App::loadBigIcon(const Row& row) {
    if (row.bigIcon)
        return;
    Bytes jpeg;
    try {
        if (!row.extracted.empty()) {
            jpeg = ExtractedGame::readIcon(row.extracted);
        } else if (row.tab != TabInstalled) {
            jpeg = readTitleIcon(*openJob(row).job, false);
        } else if (_console) {
            InstalledGame game = _console->describe(row.application);
            _console->readControl(game);
            jpeg = game.icon;
        }
    } catch (const ToolError&) {
        return;
    }
    Image decoded;
    if (jpeg.empty() || !_platform.decodeIcon(jpeg, decoded))
        return;
    auto icon = std::make_shared<Image>(decoded.scaledTo(JobIconSize, JobIconSize));
    sys::Lock lock(_mutex);
    if (Row* found = findLocked(row.key))
        found->bigIcon = icon;
    _jobIcon = icon;
    _dirty = true;
}

void App::startGame(const Row& row, const std::vector<const char*>& steps) {
    {
        sys::Lock lock(_mutex);
        _jobTitle = row.name;
        _jobIcon = iconForJob(row);
        _jobTile = row.tile;
        _jobLabel = "正在准备";
        _jobFilesDone = _jobFilesTotal = 0;
        _jobDone = _jobTotal = 0;
        _jobRate = 0;
        _jobSteps = steps;
        _jobStep = steps.empty() ? -1 : 0;
        workPlannedFor(row, _jobKind, _gamePlanned);
        _gameDoneBefore[0] = _work[0].done;
        _gameDoneBefore[1] = _work[1].done;
        _dirty = true;
    }
    loadBigIcon(row);
}

/// Goes by what's actually been got through from here on, instead of what was expected, which a game that's left out or fails never gets to.
void App::endGameLocked() {
    for (int kind = 0; kind < 2; kind++) {
        _work[kind].planned += (_work[kind].done - _gameDoneBefore[kind]) - _gamePlanned[kind];
        _gamePlanned[kind] = 0;
    }
    _gamesFinished++;
}

void App::noteWorkLocked(const char* label, i64 done) {
    static const char* const firstPass[] = {"正在删除", "正在压缩游戏数据", "正在校验游戏数据", "正在校验其他部分", "正在写入", "正在解包", "正在读取全部内容"};
    static const char* const secondPass[] = {"正在逐字节校验", "正在校验写入内容", "正在计算其哈希"};
    int kind = -1;
    for (const char* prefix : firstPass)
        kind = strncmp(label, prefix, strlen(prefix)) == 0 ? 0 : kind;
    for (const char* prefix : secondPass)
        kind = strncmp(label, prefix, strlen(prefix)) == 0 ? 1 : kind;

    double now = sys::now();
    if (_workKindNow >= 0)
        _work[_workKindNow].seconds += now - _workLastAt;
    _workLastAt = now;
    _workKindNow = kind;
    // A part that starts again from nothing is another part of the game, the same as a DLC is.
    if (kind >= 0)
        _work[kind].done += _workLabel == label && done >= _workLastDone ? done - _workLastDone : done;
    _workLabel = label;
    _workLastDone = done;
}

/// How far through it all this is, going by how much has been got through and how long the rest takes at the rate it's going. Reading a byte
/// back takes less time than compressing one, so it counts for less. Each game counts for a little on top, for all that isn't either of those.
double App::overallLocked(double* secondsLeft) {
    constexpr double AssumedFirstPass = 24.0 * (1 << 20), AssumedSecondPass = 64.0 * (1 << 20);
    // What goes with compressing a game, which has nothing to do with how much of it there is. There's nothing like that to anything else.
    const double GameOverhead = _jobKind == JobKind::Compressing ? 32.0 * (1 << 20) : 0;
    auto rateOf = [this](int kind, double assumed) {
        const Work& work = _work[kind];
        return work.done >= (16 << 20) && work.seconds > 0.5 ? (double)work.done / work.seconds : assumed;
    };
    double first = rateOf(0, AssumedFirstPass);
    double second = rateOf(1, std::max(AssumedSecondPass, first * 2.5));
    double weight = first / second;
    double done = (double)_work[0].done + (double)_work[1].done * weight + _gamesFinished * GameOverhead;
    double total = (double)std::max(_work[0].planned, _work[0].done) + (double)std::max(_work[1].planned, _work[1].done) * weight + _jobCount * GameOverhead;
    if (total > 0)
        _overallShown = std::max(_overallShown, std::min(done / total, 1.0));
    if (secondsLeft)
        *secondsLeft = std::max(total - done, 0.0) / first;
    return _overallShown;
}

/// Which of the steps shown on the screen that belongs to, and they only ever go forward.
void App::noteStepLocked(const char* label) {
    struct Stage {
        const char* prefix;
        const char* step;
    };
    static const Stage compressing[] = {
        {"正在压缩游戏数据", StepCompress},
        {"正在写入", StepCompress},
        {"正在写入", StepCopy},
        {"正在校验写入内容", StepCheckNsp},
        {"正在计算哈希", StepHash},
        {"正在计算哈希", StepCheckNsp},
        {"正在计算其哈希", StepHash},
        {"正在逐字节校验", StepCheck},
        {"让固件接管", StepFirmware},
        {"正在读取新副本", StepFirmware},
        {"正在替换", StepSwap},
    };
    static const Stage repacking[] = {
        {"正在校验游戏数据", StepPrepare},
        {"正在校验其他部分", StepPrepare},
        {"正在写入", StepWrite},
        {"正在逐字节校验", StepCheck},
        {"让固件接管", StepFirmware},
        {"正在读取新副本", StepFirmware},
        {"正在替换", StepSwap},
    };
    const Stage* stages = _settings.compress ? compressing : repacking;
    size_t count = _settings.compress ? sizeof(compressing) / sizeof(Stage) : sizeof(repacking) / sizeof(Stage);
    for (size_t k = 0; k < count; k++) {
        if (strncmp(label, stages[k].prefix, strlen(stages[k].prefix)) != 0)
            continue;
        for (size_t i = 0; i < _jobSteps.size(); i++) {
            if (_jobSteps[i] == stages[k].step)
                _jobStep = std::max(_jobStep, (int)i);
        }
    }
}

void App::finishJob(const std::string& title, const std::string& detail) {
    logLine(title + ": " + detail);
    sys::Lock lock(_mutex);
    _resultsTitle = title;
    _resultsDetail = detail;
    _resultsReady = true;
    _dirty = true;
}

// -------------------------------------------------------------------------------------------------------------------
// Compressing and installing

/// What's there already of a game about to be installed, which it takes the place of. That isn't what anyone wants if what's there is newer.
InstalledGame App::whatsInstalled(u64 application, u32 versionComing, bool wholeGame, u64 titleId) {
    InstalledGame game;
    try {
        game = _console->describe(application);
    } catch (const ToolError&) {
        // Not installed yet.
    }
    game.applicationId = application;
    for (auto& meta : game.metas) {
        bool sameTitle = wholeGame ? meta.key.type != Cnmt::MetaAddOnContent : meta.key.id == titleId;
        if (sameTitle && meta.key.version > versionComing)
            throw ToolError(format("主机上已安装更新的版本（v%u，而此文件为 v%u）", meta.key.version, versionComing));
    }
    return game;
}

Replacer App::replacer() { return Replacer(*_console, _platform.dataFolder() + "/journal"); }

/// Whether this console can read what's made of a game only takes finding out the once, and before any time has gone on one. It takes
/// sigpatches compressed or not, and the only thing the test copy can't say anything about is a firmware too old for compression. False,
/// having said why, if it can't.
bool App::consoleReadsWhatsMade(const std::vector<std::string>& keys) {
    if (!_console || (!_settings.compress && !_platform.firmwareReadsCompression()))
        return true;
    std::set<Storage> needed;
    {
        sys::Lock lock(_mutex);
        for (auto& key : keys) {
            Row* found = findLocked(key);
            if (found && goesOnConsole(*found))
                needed.insert(found->tab == TabInstalled ? found->storage : installTarget());
        }
    }
    for (Storage storage : needed) {
        if (_consoleChecked.count(storage))
            continue;
        try {
            replacer().checkConsole(storage, convertOptions(), _keyset);
            _consoleChecked.insert(storage);
            logLine(format("本主机可从 %s 读取压缩游戏", storageName(storage)));
        } catch (const Cancelled&) {
            finishJob("已停止", "未做任何改动。");
            return false;
        } catch (const ToolError& e) {
            logLine(e.what());
            finishJob("没有改动任何内容", e.what());
            return false;
        }
    }
    return true;
}

void App::compress(const std::vector<std::string>& keys) {
    double started = sys::now();
    if (!consoleReadsWhatsMade(keys))
        return;

    bool stopped = false;
    std::vector<std::string> installed;
    bool savedFile = false;
    for (auto& key : keys) {
        if (_stopAfterGame) {
            stopped = true;
            break;
        }
        Row copy;
        {
            sys::Lock lock(_mutex);
            _jobIndex++;
            Row* found = findLocked(key);
            if (!found)
                continue;
            found->state = RowState::Working;
            found->message.clear();
            copy = *found;
        }
        bool frees = copy.tab == TabInstalled && replacesInstalled();
        startGame(copy, stepsFor(copy));
        logLine(std::string(verbing()) + " " + copy.name);

        ResultLine result = resultFor(copy);
        bool leftOut = false;
        RowState state = RowState::Done;
        try {
            compressOne(copy, result, leftOut);
            if (!leftOut && copy.tab != TabInstalled && copy.extracted.empty() && goesOnConsole(copy))
                installed.push_back(copy.key);
        } catch (const Cancelled&) {
            stopped = true;
            state = RowState::Ready;
        } catch (const ToolError& e) {
            state = RowState::Failed;
            result.message = e.what();
            logLine(copy.name + ": " + result.message);
        } catch (const std::bad_alloc&) {
            state = RowState::Failed;
            result.message = _memoryLimited ? "当前可用内存不足，无法处理该游戏。请在启动任意游戏时按住 R 键，从自制程序菜单中打开本程序以使用全部内存" : NotEnoughMemory;
            logLine(copy.name + ": " + result.message);
        }

        savedFile = savedFile || (state == RowState::Done && !leftOut && !goesOnConsole(copy));

        // However it went, what's on the disks is different now, and a game with DLC may have got part of the way.
        i64 nowBytes = -1;
        try {
            updateFreeSpace();
            if (_console && frees && state != RowState::Done)
                nowBytes = _console->describe(copy.application).totalBytes();
        } catch (const ToolError&) {
        }

        sys::Lock lock(_mutex);
        Row* found = findLocked(key);
        bool partly = nowBytes >= 0 && nowBytes != copy.now;
        if (found) {
            found->state = stopped || leftOut ? RowState::Ready : state;
            if (partly) {
                found->now = nowBytes;
                _listChanged = true;
            }
            if (leftOut) {
                judgeLocked(*found, convertOptions());
            } else if (state == RowState::Failed) {
                found->message = result.message;
            } else if (state == RowState::Done) {
                found->ticked = false;
                found->byHand = false;
                found->belowMinimum.clear();
                found->freed = frees ? result.before - result.after : 0;
            }
        }
        if (frees && (state == RowState::Done || partly))
            _jobFreed += result.before - (partly ? nowBytes : result.after);
        if (!stopped) {
            result.state = leftOut ? RowState::Ready : state;
            if (leftOut)
                result.outcome = "已排除";
            else if (state == RowState::Done)
                result.outcome = frees || goesOnConsole(copy) ? format("压缩后 %s", formatSize(result.after).c_str()) : format("保存为 %s", formatSize(result.after).c_str());
            else
                result.outcome = partly ? "部分完成" : "未改动";
            _results.push_back(result);
            endGameLocked();
        }
        _dirty = true;
        if (stopped)
            break;
    }

    if (_rescanInstalled.exchange(false))
        _scanned[TabInstalled] = false;
    // What's been saved belongs on the list of game files.
    if (savedFile)
        forgetFileScan();
    closeOpenJob();

    std::string detail = compressSummary(sys::now() - started);
    if (_settings.deleteInstalled && !installed.empty())
        deleteInstalledFiles(installed, detail);
    saveList();
    bool anyFailed = false, anyFinished = false;
    {
        sys::Lock lock(_mutex);
        for (auto& r : _results) {
            anyFailed = anyFailed || r.state == RowState::Failed;
            anyFinished = anyFinished || r.state == RowState::Done;
        }
        if (_cacheChanged)
            saveCacheLocked();
        // What's been done goes out of the way, and the next thing worth doing is what's in front of whoever comes back to the list.
        sortRowsLocked();
        if (anyFinished) {
            _focus[_tab] = 0;
            _scroll[_tab] = 0;
        }
    }
    try {
        updateFreeSpace();
    } catch (const ToolError&) {
    }
    finishJob(stopped ? "已停止" : anyFailed ? "已完成，但有问题" : "已完成", detail);
}

/// How it went, in a sentence.
std::string App::compressSummary(double seconds) {
    int finished = 0, failed = 0, left = 0;
    i64 freed;
    {
        sys::Lock lock(_mutex);
        for (auto& r : _results) {
            if (r.state == RowState::Done)
                finished++;
            else if (r.state == RowState::Failed)
                failed++;
            else
                left++;
        }
        freed = _jobFreed;
    }
    std::string detail = finished == 1 ? format("1 个游戏已%s", verbed()) : format("%d 个游戏已%s", finished, verbed());
    if (left > 0)
        detail += format("，%d 个收益过低已跳过", left);
    if (failed > 0)
        detail += format("，%d 个未能完成", failed);
    if (freed > 0)
        detail += format("，已释放 %s", formatSize(freed).c_str());
    detail += format("，压缩用时 %s。", formatElapsed(seconds).c_str());
    return detail;
}

void App::compressOne(const Row& row, ResultLine& result, bool& leftOut) {
    ConvertOptions options = convertOptions();
    checkStop();

    // Installing something that's already compressed only takes copying it across.
    if (row.tab != TabInstalled && goesOnConsole(row))
        options.keepCompressed = true;

    // A title nobody has analyzed yet gets its answer now, so the time doesn't go on one that gains too little.
    SizeEstimate estimate = row.estimate;
    if (!row.hasEstimate) {
        estimate = estimateRow(row);
        sys::Lock lock(_mutex);
        // Now it's known to be compressed already, the screen says it's only being copied across.
        if (estimate.alreadyCompressed && options.keepCompressed) {
            for (auto& step : _jobSteps)
                step = step == StepCompress ? StepCopy : step;
        }
        if (Row* found = findLocked(row.key)) {
            found->hasEstimate = true;
            found->estimate = estimate;
            rememberEstimateLocked(*found);
        }
    }
    // Asking for it to be installed is asking for it to be installed, whatever it would gain.
    if (!row.byHand && !_installNow) {
        Row judged = row;
        judged.hasEstimate = true;
        judged.estimate = estimate;
        {
            sys::Lock lock(_mutex);
            judgeLocked(judged, options);
        }
        if (!judged.belowMinimum.empty()) {
            leftOut = true;
            result.message = judged.belowMinimum;
            return;
        }
    }

    if (row.tab == TabInstalled)
        compressInstalled(row, estimate, options, result);
    else if (!row.extracted.empty())
        buildExtracted(row, estimate, options, result);
    else
        compressFiles(row, estimate, options, result);
}

/// A little over what a game is expected to come to, which decides what kind of file it gets, and how much room is set aside for it.
static i64 sizeHintFor(const SizeEstimate& estimate) {
    i64 expected = estimate.outputBytes + estimate.marginBytes;
    return expected + expected / 16;
}

void App::compressInstalled(const Row& row, const SizeEstimate& estimate, const ConvertOptions& options, ResultLine& result) {
    if (!replacesInstalled()) {
        std::string folder = nspFolder();
        needRoomIn(folder, estimate.outputBytes + estimate.marginBytes);
        auto loaded = loadGame(row.application);
        for (auto& job : loaded->set->jobs) {
            std::string path = joinPath(folder, outputFileName(job, *loaded->set, options));
            NspOutput nsp(path, (int)job.contents.size(), sizeHintFor(estimate));
            TitleConverter(options, _keyset).run(job, nsp);
            result.after += nsp.outputBytes();
            logLine("已保存" + path);
        }
        return;
    }

    needRoom(row.storage, estimate.outputBytes);
    // A title at a time, reading the console afresh each time, as the last one will have changed what's there.
    Replacer replacing = replacer();
    std::set<u64> done;
    while (true) {
        auto loaded = loadGame(row.application);
        const TitleJob* job = nullptr;
        for (auto& candidate : loaded->set->jobs) {
            if (!done.count(candidate.main->meta.titleId)) {
                job = &candidate;
                break;
            }
        }
        if (!job)
            break;
        done.insert(job->main->meta.titleId);
        replacing.run(loaded->game, *job, options, _keyset);
    }
    InstalledGame now = _console->describe(row.application);
    result.after = now.totalBytes();

    // It's now what it is, and there's no need to go looking on the next start to find that out.
    sys::Lock lock(_mutex);
    if (Row* found = findLocked(row.key)) {
        applyContents(*found, now);
        found->estimate = SizeEstimate{result.after, result.after, 0, 0, 0, options.compress};
        found->hasEstimate = true;
        rememberEstimateLocked(*found);
        _listChanged = true;
    }
}

void App::buildExtracted(const Row& row, const SizeEstimate& estimate, const ConvertOptions& options, ResultLine& result) {
    ExtractedGame game = openExtracted(row.extracted);
    if (goesOnConsole(row)) {
        needRoom(installTarget(), estimate.outputBytes);
        InstalledGame there = whatsInstalled(row.application, game.version, true, game.titleId);
        result.after = replacer().run(there, game, options, _keyset, installTarget()).after;
        _rescanInstalled = true;
        return;
    }
    std::string folder = nspFolder();
    needRoomIn(folder, estimate.outputBytes + estimate.marginBytes);
    std::string path = joinPath(folder, game.outputFileName(options));
    NspOutput nsp(path, game.contentCount(), sizeHintFor(estimate));
    TitleConverter(options, _keyset).build(game, nsp);
    result.after = nsp.outputBytes();
    logLine("已保存" + path);
}

/// The game first and then each of its DLC.
void App::compressFiles(const Row& row, const SizeEstimate& estimate, const ConvertOptions& options, ResultLine& result) {
    OpenedJobs opened = openJobs(row);
    bool installing = goesOnConsole(row);
    std::string folder = nspFolder();
    if (installing)
        needRoom(installTarget(), estimate.outputBytes);
    else
        needRoomIn(folder, estimate.outputBytes + estimate.marginBytes);
    for (const TitleJob* job : opened.jobs) {
        checkStop();
        if (installing) {
            u32 version = job->update ? job->update->meta.version : job->main->meta.version;
            InstalledGame game = whatsInstalled(row.application, version, job->isBaseGame(), job->main->meta.titleId);
            result.after += replacer().run(game, *job, options, _keyset, installTarget()).after;
            _rescanInstalled = true;
            continue;
        }
        // What's already compressed stays where it is, as it is.
        if (isAlreadyCompressed(*job, options)) {
            result.after += job->inputBytes;
            continue;
        }
        std::string name = joinPath(folder, outputFileName(*job, *opened.set, options));
        // What's there already is somebody's, and may well be what this is being made from.
        std::string path = name;
        for (int copy = 1; sys::exists(path); copy++)
            path = name.substr(0, name.size() - 4) + (copy == 1 ? std::string(" (repacked).nsp") : format(" (repacked %d).nsp", copy));
        NspOutput nsp(path, (int)job->contents.size(), sizeHintFor(estimate));
        TitleConverter(options, _keyset).run(*job, nsp);
        result.after += nsp.outputBytes();
        logLine("已保存" + path);
    }
}

/// A file goes once everything in it has gone onto the console, and not before.
void App::deleteInstalledFiles(const std::vector<std::string>& installed, std::string& detail) {
    // A file that's still open can't be deleted.
    closeOpenJob();

    std::set<std::string> paths;
    {
        sys::Lock lock(_mutex);
        std::vector<const Row*> done;
        std::set<std::string> candidates;
        for (auto& row : _rows) {
            if (std::find(installed.begin(), installed.end(), row.key) == installed.end())
                continue;
            done.push_back(&row);
            candidates.insert(row.mainFile);
            if (!row.updateFile.empty())
                candidates.insert(row.updateFile);
            candidates.insert(row.dlcFiles.begin(), row.dlcFiles.end());
        }
        auto covers = [](const Row* row, const std::string& path, const IndexedTitle& title) {
            if ((row->mainFile == path && row->titleId == title.id) || (row->updateFile == path && title.type == Cnmt::MetaPatch))
                return true;
            for (size_t i = 0; i < row->dlcFiles.size(); i++) {
                if (row->dlcFiles[i] == path && row->dlcIds[i] == title.id)
                    return true;
            }
            return false;
        };
        for (auto& path : candidates) {
            auto known = _fileIndex.find(path);
            bool allDone = known != _fileIndex.end() && std::all_of(known->second.titles.begin(), known->second.titles.end(), [&](const IndexedTitle& title) {
                return std::any_of(done.begin(), done.end(), [&](const Row* row) { return covers(row, path, title); });
            });
            if (allDone)
                paths.insert(path);
            else
                logLine("已保留" + path + "：其中其他游戏尚未安装");
        }
        for (auto& row : _rows) {
            if (row.tab == TabFiles && row.extracted.empty())
                row.ready = false;
        }
    }
    forgetFileScan();

    int deleted = 0;
    for (auto& path : paths) {
        if (sys::removeFile(path)) {
            deleted++;
            logLine("已删除" + path);
        } else {
            logLine("无法删除" + path);
        }
    }
    sys::commit(nspFolder());
    if (deleted > 0)
        detail += deleted == 1 ? "，已删除 1 个文件。" : format("，已删除 %d 个文件。", deleted);
}

// -------------------------------------------------------------------------------------------------------------------
// Extracting, checking and removing

App::ResultLine App::resultFor(const Row& row) {
    ResultLine result;
    result.name = row.name;
    result.before = row.now;
    result.icon = row.icon;
    result.tile = row.tile;
    return result;
}

/// Each game in turn, for what doesn't need anything more than doing something to it and saying how that went.
void App::workThrough(const std::vector<std::string>& keys, const char* whenItFails, const std::function<void(const Row&, ResultLine&)>& work, const std::function<std::string(int, double)>& summary) {
    double started = sys::now();
    bool stopped = false;
    int failed = 0;
    for (auto& key : keys) {
        if (_stopAfterGame) {
            stopped = true;
            break;
        }
        Row copy;
        {
            sys::Lock lock(_mutex);
            _jobIndex++;
            Row* found = findLocked(key);
            if (!found)
                continue;
            copy = *found;
        }
        startGame(copy, {});
        ResultLine result = resultFor(copy);
        auto fail = [&](const std::string& why) {
            failed++;
            result.state = RowState::Failed;
            result.outcome = whenItFails;
            result.message = why;
            logLine(copy.name + ": " + why);
        };
        try {
            result.state = RowState::Done;
            work(copy, result);
        } catch (const Cancelled&) {
            stopped = true;
        } catch (const ToolError& e) {
            fail(e.what());
        } catch (const std::bad_alloc&) {
            fail(NotEnoughMemory);
        }
        if (stopped)
            break;
        sys::Lock lock(_mutex);
        _results.push_back(result);
        endGameLocked();
    }
    finishJob(stopped ? "已停止" : failed > 0 ? "已完成，但有问题" : "已完成", summary(failed, sys::now() - started));
}

/// What a game's work is done on: the titles of an installed game, or of the files of a game file's row with its DLC.
void App::forEachTitle(const Row& row, const std::function<void(const TitleJob&, const TitleSet&)>& work) {
    if (row.tab == TabInstalled) {
        auto loaded = loadGame(row.application);
        for (auto& job : loaded->set->jobs)
            work(job, *loaded->set);
    } else {
        OpenedJobs opened = openJobs(row);
        for (auto* job : opened.jobs)
            work(*job, *opened.set);
    }
}

void App::extract(const std::vector<std::string>& keys) {
    std::string output = extractFolder();
    workThrough(keys, "未解包", [&](const Row& row, ResultLine& result) {
        sys::makeDirectories(output);
        if (!sys::isDirectory(output))
            throw ToolError("文件夹" + output + "无法访问。如果在 USB 可移动设备上，请确认已插好。");
        i64 free = sys::freeSpace(output);
        if (free < row.now + RoomToSpare)
            throw ToolError(format("至少需要 %s 可用空间于 %s，目前只有 %s", formatSize(row.now + RoomToSpare).c_str(), output.c_str(), formatSize(free).c_str()));
        i64 bytes = 0;
        forEachTitle(row, [&](const TitleJob& job, const TitleSet& set) {
            // There's no use in doing it again over what's there, and half a game left behind would look like a whole one on the list.
            std::string folder = extractedFolderOf(job, set, output);
            if (ExtractedGame::isOne(folder))
                throw ToolError("它已位于" + folder + "。如需再次解包，请先在「游戏文件」中删除它；若已隐藏，可在 + 菜单中显示已完成的游戏");
            // Whatever's left of a folder that isn't a whole game is no use to anyone.
            sys::removeTree(folder);
            try {
                bytes += extractTitle(job, set, output).bytes;
            } catch (...) {
                sys::removeTree(folder);
                throw;
            }
        });
        result.outcome = format("%s 已解包", formatSize(bytes).c_str());
        result.after = bytes;
    }, [&](int, double seconds) { return format("已解包到 %s，用时 %s。", output.c_str(), formatElapsed(seconds).c_str()); });
    closeOpenJob();
    forgetFileScan();
}

void App::check(const std::vector<std::string>& keys) {
    std::string temp = _platform.dataFolder() + "/temp";
    workThrough(keys, "出现问题", [&](const Row& row, ResultLine& result) {
        sys::makeDirectories(temp);
        forEachTitle(row, [&](const TitleJob& job, const TitleSet&) { checkTitle(job, temp); });
        result.outcome = "文件完整";
    }, [&](int failed, double seconds) {
        std::string what = failed == 0 ? "所有游戏回读校验均正常" : failed == 1 ? "有 1 个游戏回读校验异常" : format("有 %d 个游戏回读校验异常", failed);
        return format("%s，用时 %s。", what.c_str(), formatElapsed(seconds).c_str());
    });
    closeOpenJob();
}

/// Takes a game off the console, or its files off wherever they are. What people have played stays: saves aren't part of a game.
void App::remove(const std::vector<std::string>& keys) {
    std::vector<std::string> paths, removed;
    workThrough(keys, "未移除", [&](const Row& row, ResultLine& result) {
        sink().progress("正在移除", 0, 1, 0);
        // A file that's open can't be deleted.
        closeOpenJob();
        if (row.tab == TabInstalled) {
            uninstall(row);
            result.outcome = "已卸载";
        } else if (!row.extracted.empty()) {
            deleteFolder(row.extracted);
            paths.push_back(row.extracted);
            result.outcome = "已删除";
        } else {
            std::vector<std::string> files{row.mainFile, row.updateFile};
            files.insert(files.end(), row.dlcFiles.begin(), row.dlcFiles.end());
            for (auto& path : files) {
                if (path.empty() || std::find(paths.begin(), paths.end(), path) != paths.end())
                    continue;
                if (!sys::removeFile(path) && sys::exists(path))
                    throw ToolError("'" + fileNameOf(path) + "' 无法删除");
                paths.push_back(path);
            }
            result.outcome = "已删除";
        }
        result.after = 0;
        removed.push_back(row.key);
        logLine(result.outcome + " " + row.name);
    }, [&](int failed, double seconds) { return failed == 0 ? format("已完成，用时 %s。", formatElapsed(seconds).c_str()) : failed == 1 ? std::string("其中一项保持原样未动。") : format("其中 %d 个保持原样未改动。", failed); });

    updateFreeSpace();
    // However it went, what the lists say has to go by what's there now.
    {
        sys::Lock lock(_mutex);
        for (auto& key : keys) {
            Row* found = findLocked(key);
            if (found && !found->extracted.empty() && !sys::exists(found->extracted) && std::find(removed.begin(), removed.end(), key) == removed.end()) {
                removed.push_back(key);
                paths.push_back(found->extracted);
            }
        }
    }
    forgetFileScan();
    if (removed.empty())
        return;
    {
        sys::Lock lock(_mutex);
        _rows.erase(std::remove_if(_rows.begin(), _rows.end(), [&](const Row& r) { return std::find(removed.begin(), removed.end(), r.key) != removed.end(); }), _rows.end());
        for (auto& path : paths) {
            _fileIndex.erase(path);
            _fileIndexChanged = true;
        }
        _listChanged = true;
        _dirty = true;
    }
    saveList();
    if (!paths.empty())
        saveFileIndex();
}

void App::uninstall(const Row& row) {
    InstalledGame game = _console->describe(row.application);
    if (!game.blocked.empty())
        throw ToolError("该项正在使用中：可能当前已打开，或本程序是通过它启动的。请关闭后重试");
    // In this order, so that a power cut part of the way through can't leave the home menu with a game that isn't there.
    _console->setRecords(row.application, {});
    std::set<std::string> gone;
    for (auto& meta : game.metas) {
        if (meta.storage == Storage::GameCard)
            continue;
        _console->removeMeta(meta.storage, meta.key);
        for (auto& content : meta.contents) {
            if (gone.insert(content.id.hex()).second && _console->hasContent(meta.storage, content.id))
                _console->deleteContent(meta.storage, content.id);
        }
    }
}

/// The folder of an extracted game, a file at a time, as with thousands of them, all at once would look like it had got stuck.
void App::deleteFolder(const std::string& path) {
    // Whatever the list says, this only ever goes for what's plainly a game.
    if (!ExtractedGame::isOne(path))
        throw ToolError("此文件夹已不像游戏目录，因此未作处理");
    std::vector<std::string> folders{path};
    std::vector<std::pair<std::string, i64>> files;
    i64 total = 0;
    for (size_t at = 0; at < folders.size(); at++) {
        sink().counting("正在查找其文件", (i64)files.size());
        std::string folder = folders[at];
        for (auto& entry : sys::list(folder)) {
            std::string inside = folder + "/" + entry.name;
            if (entry.isDirectory) {
                folders.push_back(std::move(inside));
            } else {
                files.emplace_back(std::move(inside), entry.size);
                total += entry.size;
            }
        }
    }
    Progress progress("正在删除其文件", total);
    i64 done = 0;
    i64 gone = 0;
    for (auto& file : files) {
        sys::removeFile(file.first);
        done += file.second;
        progress.report(done);
        sink().files(++gone, (i64)files.size());
    }
    progress.done();
    for (size_t at = folders.size(); at-- > 0;)
        sys::removeDirectory(folders[at]);
    if (sys::exists(path))
        sys::removeTree(path);
    if (sys::exists(path))
        throw ToolError("此文件夹有部分内容无法删除");
}

}  // namespace hbp
