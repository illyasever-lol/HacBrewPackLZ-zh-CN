// The work: analyzing what each game would gain, and compressing, installing, extracting, checking and removing games, with how far
// along it all is.
#include "app_internal.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <new>

#include "../core/remote.h"

namespace hbp {

using namespace app;

namespace {

constexpr const char* StepCompress = "压缩";
/// Reading all of it, writing nothing, for a game a first look can't vouch for, before anything of it is written.
constexpr const char* StepMeasure = "Measure it";
/// Installing what's already compressed only copies it across, without compressing any of it again.
constexpr const char* StepCopy = "复制过去";
constexpr const char* StepPrepare = "检查内容";
constexpr const char* StepWrite = "写入";
constexpr const char* StepCheck = "逐字节校验";
constexpr const char* StepHash = "为主机准备文件";
constexpr const char* StepCheckNsp = "校验写入内容";
constexpr const char* StepFirmware = "回读";
constexpr const char* StepSwap = "替换";

const char* const NotEnoughMemory = "内存不足，无法处理此项";
const char* const Unexpected = "发生意外错误：";

/// What the heap was like the last time memory ran out, as it ran out, before anything was let go. The handler that takes it can't allocate.
std::atomic<i64> g_ranOutInUse{-1};
std::atomic<i64> g_ranOutFree{-1};
std::atomic<int> g_ranOutTimes{0};

void ranOut() {
    sys::HeapUse use = sys::heapUse(false);
    g_ranOutInUse = use.inUse;
    g_ranOutFree = use.free;
    g_ranOutTimes++;
    throw std::bad_alloc();
}

}  // namespace

void App::watchMemory() { std::set_new_handler(ranOut); }

std::string App::memoryNote() {
    sys::HeapUse after = sys::heapUse(true);
    if (after.inUse < 0)
        return std::string();
    std::string label;
    {
        sys::Lock lock(_mutex);
        label = _jobLabel;
    }
    std::string note = "内存耗尽";
    if (!label.empty())
        note += " during \"" + label + "\"";
    // Ran out of more than once, it's the last time that's said.
    if (g_ranOutTimes.exchange(0) > 0)
        note += format("，已用 %s，可用 %s", formatSize(g_ranOutInUse).c_str(), formatSize(g_ranOutFree).c_str());
    note += format("；随后已用 %s，可用 %s，其中连续空间 %s", formatSize(after.inUse).c_str(), formatSize(after.free).c_str(), formatSize(after.largestPiece).c_str());
    if (std::string where = sys::refusedAllocations(); !where.empty())
        note += "; " + where;
    return note;
}

struct App::LoadedGame {
    InstalledGame game;
    std::shared_ptr<InstalledArchive> archive;
    std::unique_ptr<TitleSet> set;
};

std::unique_ptr<App::LoadedGame> App::loadGame(u64 application) {
    auto loaded = std::make_unique<LoadedGame>();
    loaded->game = _console->describe(application);
    // An update newer than a copy this rebuilt is made part of it. One that isn't would be laid over data it was never made for.
    judgeUpdateOver(loaded->game);
    if (loaded->game.blocked == UpdateNoNewer)
        throw ToolError(UpdateNoNewer);
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

/// A copy an older version made is worked out like any other game, as made again it can come out smaller, and it says what compressing it freed.
static void noteOlderCopy(const TitleJob& job, const ConvertOptions& options, SizeEstimate& estimate) {
    if (!options.improveOlderCopies || job.update)
        return;
    if (isMadeByThisTool(job)) {
        estimate.olderCopy = true;
        estimate.freedBytes = freedBy(job, options);
    } else if (hasCompressedGameData(job)) {
        // Its developer compressed it, and what's worked out is what making it again here would come to.
        estimate.byMakers = true;
    }
}

/// There's no working out what a game would gain if all of it is already stored the way this would store it. A copy this tool made still
/// says what compressing it freed. A copy an older version made, or one its developer compressed, is worked out like any other game.
SizeEstimate App::estimateJob(const TitleJob& job, const ConvertOptions& options) {
    if (isAlreadyCompressed(job, options))
        return alreadyCompressedEstimate(job, options);
    SizeEstimate estimate = estimateTitle(job, options);
    noteOlderCopy(job, options, estimate);
    return estimate;
}

SizeEstimate App::measureJob(const TitleJob& job, const ConvertOptions& options) {
    if (isAlreadyCompressed(job, options)) {
        SizeEstimate same = alreadyCompressedEstimate(job, options);
        same.measured = true;
        return same;
    }
    SizeEstimate estimate = measureTitle(job, options);
    noteOlderCopy(job, options, estimate);
    return estimate;
}

/// What the PC sharing a game says it comes to, worked out the same way, with these settings. False if it can't say, as an older version
/// can't, and it's to be worked out here. Measured, the PC reads all of it, far faster than across the network. A PC from before that answers
/// with its first look, which says it isn't measured.
bool App::estimateOnPc(const Row& row, SizeEstimate& estimate, bool measure) {
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
    if (measure)
        settings += "\t1";
    std::string answer;
    try {
        if (!remote::estimate(paths, settings, answer, [] { checkStop(); }))
            return false;
    } catch (const ToolError&) {
        return false;
    }
    long long in = 0, out = 0, margin = 0, romFs = 0, stored = 0, freed = 0;
    int compressed = 0, measured = 0, byMakers = 0;
    if (sscanf(answer.c_str(), "%lld %lld %lld %lld %lld %d %d %lld %d", &in, &out, &margin, &romFs, &stored, &compressed, &measured, &freed, &byMakers) < 6)
        return false;
    estimate = SizeEstimate();
    estimate.measured = measured != 0;
    estimate.freedBytes = freed;
    estimate.byMakers = byMakers != 0;
    // What a packed file takes installed as it is, which an older PC doesn't say.
    long long skip = 0, installed = 0;
    int flag = 0;
    if (sscanf(answer.c_str(), "%lld %lld %lld %lld %lld %d %d %lld %d %lld", &skip, &skip, &skip, &skip, &skip, &flag, &flag, &skip, &flag, &installed) == 10 &&
        installed > 0) {
        estimate.packedBytes = row.now;
        estimate.installedBytes = installed + std::max<i64>(row.now - in, 0);
    }
    estimate.inputBytes = row.now;
    // Whatever the PC didn't go through stays just as big as it is, the same as here.
    estimate.outputBytes = out + std::max<i64>(row.now - in, 0);
    estimate.marginBytes = margin;
    estimate.romFsBytes = romFs;
    estimate.romFsStoredBytes = stored;
    estimate.alreadyCompressed = compressed != 0;
    logLine(format(estimate.measured ? "已在电脑上测量：%s" : "已在电脑上分析：%s", row.name.c_str()));
    return true;
}

SizeEstimate App::estimateRow(const Row& row) { return workOutRow(row, false); }

SizeEstimate App::measureRow(const Row& row) { return workOutRow(row, true); }

SizeEstimate App::workOutRow(const Row& row, bool measure) {
    ConvertOptions options = convertOptions();
    // On the console, a copy an older version made, or one its developer compressed, can come out smaller made again.
    options.improveOlderCopies = row.tab == TabInstalled;
    // A game under 1 GB is measured exactly, every block compressed: it takes little longer, and a margin would be most of what it saves.
    if (measure && row.now < MeasureExactlyBelow)
        options.measureEvery = 1;
    std::string where = !row.extracted.empty() ? row.extracted : row.mainFile;
    // A PC sharing the game works it out far faster than reading samples of it across the network would, and reading all of it far faster still.
    SizeEstimate fromPc;
    if (row.tab != TabInstalled && remote::isRemote(where) && estimateOnPc(row, fromPc, measure) && fromPc.measured == measure)
        return fromPc;
    // Only a USB drive can be a hard drive. A large game on the SD card can be slow to read as well, but that's not from going looking.
    auto drives = _platform.drives();
    options.noSeekTime = std::none_of(drives.begin(), drives.end(), [&](const Drive& drive) { return drive.isUsb && startsWithNoCase(where, drive.root); });
    if (!row.extracted.empty())
        return estimateBuild(openExtracted(row.extracted), options, measure);
    if (row.tab != TabInstalled && row.dlcFiles.empty()) {
        // Held for as long as it's read: a first look taken in the middle of reading it in full opens another file, which the one kept open for
        // the next time gives way to.
        OpenedJob opened = openJob(row);
        const TitleJob& job = *opened.job;
        // What's installed may be a copy this compressed, which an update doesn't always fit.
        if (row.overInstalled)
            checkUpdateFits(job, false);
        return measure ? measureJob(job, options) : estimateJob(job, options);
    }

    // A base game with its update and each DLC come to one row.
    std::vector<const TitleJob*> jobs;
    std::shared_ptr<void> keep;
    if (row.tab != TabInstalled) {
        OpenedJobs opened = openJobs(row);
        jobs = opened.jobs;
        keep = opened.set;
    } else {
        auto loaded = loadGame(row.application);
        for (auto& job : loaded->set->jobs) {
            // An update that went in over a copy this compressed is made part of it, if it fits.
            if (loaded->game.updateOverCopy)
                checkUpdateFits(job, true);
            jobs.push_back(&job);
        }
        keep = std::shared_ptr<LoadedGame>(std::move(loaded));
    }
    SizeEstimate total;
    double margin = 0;
    i64 covered = 0;
    // DLC with no game data to it says nothing either way.
    bool anyCompressed = false, anyNot = false, anyByMakers = false, anyPacked = false;
    for (const TitleJob* job : jobs) {
        SizeEstimate one = measure ? measureJob(*job, options) : estimateJob(*job, options);
        anyCompressed = anyCompressed || one.alreadyCompressed;
        // A part with no game data compressing would touch, such as DLC with a few hundred bytes of it, says nothing either way. Any other part
        // that isn't compressed makes a game that isn't, whatever a first look says it gains, as only reading all of it can say it gains
        // nothing: a first look under half a percent made a whole game with DLC count as compressed. A copy an older version made, or one its
        // developer compressed, is to be looked at too.
        anyNot = anyNot || one.olderCopy || (!one.alreadyCompressed && one.romFsBytes > 0);
        anyByMakers = anyByMakers || one.byMakers;
        // What's installed as it is, for a row with a packed file in it: each packed part unpacked, and the rest as it is.
        total.installedBytes += one.packedBytes > 0 ? one.installedBytes : one.inputBytes;
        anyPacked = anyPacked || one.packedBytes > 0;
        total.freedBytes += one.freedBytes;
        total.olderCopy = total.olderCopy || one.olderCopy;
        total.outputBytes += one.outputBytes;
        total.romFsBytes += one.romFsBytes;
        total.romFsStoredBytes += one.romFsStoredBytes;
        margin += (double)one.marginBytes * (double)one.marginBytes;
        covered += job->inputBytes;
    }
    total.marginBytes = (i64)sqrt(margin);
    total.alreadyCompressed = anyCompressed && !anyNot;
    total.byMakers = anyByMakers && total.freedBytes == 0;
    total.measured = measure;
    total.inputBytes = row.now;
    // Whatever couldn't be gone through stays just as big as it is.
    total.outputBytes += std::max<i64>(row.now - covered, 0);
    if (anyPacked) {
        total.packedBytes = row.now;
        total.installedBytes += std::max<i64>(row.now - covered, 0);
    } else {
        total.installedBytes = 0;
    }
    return total;
}

void App::analyze(const std::vector<std::string>& keys, bool measuring) {
    // Gone through by place, as games looked at in the middle of measuring join it (firstLooksWhileMeasuring).
    _analysisQueue = keys;
    _analysisAt = 0;
    {
        sys::Lock lock(_mutex);
        _analysisCount = (int)keys.size();
        _analysisIndex = 0;
        // Measuring goes by the game data read, as one game can be thousands of times the size of another.
        _analysisBytesTotal = _analysisBytesBefore = _analysisRowBytes = 0;
        _analysisImagesDone = _analysisImageDone = _analysisImageTotal = 0;
        for (auto& key : keys) {
            if (const Row* row = measuring ? findLocked(key) : nullptr)
                _analysisBytesTotal += row->measureBytes();
        }
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

    for (_analysisAt = 0; _analysisAt < _analysisQueue.size(); _analysisAt++) {
        // A copy, as the queue can grow while this one is worked out.
        std::string key = _analysisQueue[_analysisAt];
        checkStop();
        // A list that's come up, or games that have, get their first looks before anything more is read in full.
        if (measuring) {
            sys::Lock lock(_mutex);
            if (firstLooksLeftLocked())
                return;
        }
        // Reading games in full by itself waits while the battery's low.
        if (measuring && !batteryTooLow().empty()) {
            _measureAgainAt = sys::now() + 60;
            logLine("电量低时暂停测量");
            return;
        }
        workOutOne(key, measuring, tab);
    }
}

/// One game's first look, or reading it in full, and what it comes to.
void App::workOutOne(const std::string& key, bool measuring, int& tab) {
    Row copy;
    {
        sys::Lock lock(_mutex);
        _analysisIndex++;
        Row* found = findLocked(key);
        if (!found)
            return;
        found->state = RowState::Working;
        copy = *found;
        tab = copy.tab;
        _analysisTitle = copy.name;
        _analysisBytesBefore += _analysisRowBytes;
        _analysisRowBytes = measuring ? copy.measureBytes() : 0;
        _analysisImagesDone = _analysisImageDone = _analysisImageTotal = 0;
        _dirty = true;
    }

    std::string failure;
    bool mayWorkNextTime = false;
    SizeEstimate estimate;
    double analyzeStarted = sys::now();
    try {
        logLine((measuring ? "测量中 " : "正在分析 ") + copy.name);
        // For the tests, measuring slowly enough to be seen going on, giving way and taking first looks in the middle, as reading does, and a
        // first look slowly enough for something to be done while it's taken.
        if (const char* slow = getenv(measuring ? "HBP_TEST_SLOW_MEASURE" : "HBP_TEST_SLOW_LOOK"); slow && *slow) {
            for (int waited = 0; waited < atoi(slow); waited += 10) {
                checkStop();
                firstLooksWhileMeasuring();
                sys::sleepMs(10);
            }
        }
        estimate = measuring ? measureRow(copy) : estimateRow(copy);
        logLine(format("%s took %.2f s", copy.name.c_str(), sys::now() - analyzeStarted));
    } catch (const ToolError& e) {
        failure = withNextStep(e.what());
        // A drive that's been taken out can come back, where a game with no key for it will still have none.
        // Keys dumped again, or a ticket installed since, can change it too, and a game that can't be read for that is turned down straight away.
        mayWorkNextTime = failure.find("Couldn't read") != std::string::npos || failure.find("being looked through") != std::string::npos || wantsKeys(failure);
        logLine(copy.name + ": " + failure);
    } catch (const std::bad_alloc&) {
        failure = NotEnoughMemory;
        mayWorkNextTime = true;
        logLine(copy.name + ": " + failure);
        if (std::string note = memoryNote(); !note.empty())
            logLine(note);
    } catch (const Cancelled&) {
        sys::Lock lock(_mutex);
        if (Row* found = findLocked(key))
            found->state = RowState::Ready;
        throw;
    }

    sys::Lock lock(_mutex);
    // How fast this console measures, which the time left goes by, from each game read in full.
    if (measuring && failure.empty()) {
        _measuredBytes[copy.measuredExactly() ? 1 : 0] += (double)copy.measureBytes();
        _measuredSeconds[copy.measuredExactly() ? 1 : 0] += sys::now() - analyzeStarted;
    }
    Row* found = findLocked(key);
    if (!found)
        return;
    found->state = failure.empty() ? RowState::Ready : RowState::Failed;
    found->message = failure;
    if (failure.empty()) {
        // Where it is on the list on the screen, or would be: how many are shown above it. The pointer stays on the game it's on, whether
        // this one goes from the list above it or comes onto it there, unless it's at the top, which is where the best one is.
        int above = 0;
        for (auto& row : _rows) {
            if (&row == found)
                break;
            above += row.tab == _tab && !leftOffListLocked(row) ? 1 : 0;
        }
        bool wasShown = !leftOffListLocked(*found);
        found->hasEstimate = true;
        found->estimate = estimate;
        found->measureAgain = false;
        // The options may have been changed by now.
        judgeLocked(*found, convertOptions());
        if (found->tab == TabInstalled)
            judgeFilesOfLocked(found->application);
        rememberEstimateLocked(*found);
        _resortWanted = true;
        bool shown = !leftOffListLocked(*found);
        if (found->tab == _tab && _tab < TabCount && shown != wasShown) {
            if (wasShown && above < _focus[_tab])
                _focus[_tab]--;
            else if (shown && above <= _focus[_tab] && _focus[_tab] > 0)
                _focus[_tab]++;
        }
    } else {
        if (!found->byHand)
            found->ticked = false;
        // Reading a game in full that turns out damaged would only go the same way each time the app opened, after minutes of reading,
        // so that's remembered too, until it's analyzed again by hand or the game changes.
        if (!mayWorkNextTime)
            rememberFailureLocked(*found, failure);
    }
    _dirty = true;
}

/// Reading a game in full can take minutes, and given way to, that game starts over. A game that needs a first look meanwhile, asked to be
/// worked out again or just come onto a list, gets it there and then, with the reading waiting where it is, and what the screen says about
/// the reading put back afterwards. Only between one part of what the analyst reads and the next, and never from inside a first look.
void App::firstLooksWhileMeasuring() {
    if (!_analystMeasuring || _lookingWhileMeasuring)
        return;
    double now = sys::now();
    if (now - _firstLooksCheckedAt < 0.25)
        return;
    _firstLooksCheckedAt = now;
    std::vector<std::string> keys;
    int lookTab = -1;
    {
        sys::Lock lock(_mutex);
        // The list on the screen first, as when they're taken by themselves.
        for (int i = 0; i < TabCount && keys.empty(); i++) {
            int each = _tab >= 0 && _tab < TabCount ? (_tab + i) % TabCount : i;
            if (_scanned[each] && _autoAnalyze[each] && _scanningTab != each) {
                keys = keysToAnalyzeLocked(each);
                lookTab = each;
            }
        }
    }
    if (keys.empty())
        return;

    struct Shown {
        int index, count, tab;
        std::string title;
        i64 bytesTotal, bytesBefore, rowBytes, imagesDone, imageDone, imageTotal;
    };
    Shown shown;
    {
        sys::Lock lock(_mutex);
        shown = Shown{_analysisIndex, _analysisCount, _analyzingTab.load(), _analysisTitle, _analysisBytesTotal, _analysisBytesBefore,
                      _analysisRowBytes, _analysisImagesDone, _analysisImageDone, _analysisImageTotal};
        // The footer goes on showing the reading as it was, as the looks take a second each and the games in their rows say so.
        _measuringShown = MeasuringShown{_analysisTitle, _analysisIndex, _analysisCount, _analyzingTab.load(), analysisFractionLocked()};
        _lookingWhileMeasuring = true;
        _analysisIndex = 0;
        _analysisCount = (int)keys.size();
        _analysisBytesTotal = _analysisBytesBefore = _analysisRowBytes = 0;
        _analysisImagesDone = _analysisImageDone = _analysisImageTotal = 0;
        _analyzingTab = lookTab;
        _analystMeasuring = false;
        _dirty = true;
    }
    // However they end, even stopped, the reading is what's shown again.
    struct Restore {
        App& app;
        const Shown& shown;
        ~Restore() {
            sys::Lock lock(app._mutex);
            app._analysisIndex = shown.index;
            app._analysisCount = shown.count;
            app._analysisTitle = shown.title;
            app._analysisBytesTotal = shown.bytesTotal;
            app._analysisBytesBefore = shown.bytesBefore;
            app._analysisRowBytes = shown.rowBytes;
            app._analysisImagesDone = shown.imagesDone;
            app._analysisImageDone = shown.imageDone;
            app._analysisImageTotal = shown.imageTotal;
            app._analyzingTab = shown.tab;
            app._analystMeasuring = true;
            app._lookingWhileMeasuring = false;
            app._dirty = true;
        }
    } restore{*this, shown};
    logLine("测量中 " + shown.title + " waits for " + (keys.size() == 1 ? std::string("初览") : format("%d 次初览", (int)keys.size())));
    int tab = -1;
    for (auto& key : keys) {
        checkStop();
        workOutOne(key, false, tab);
    }
    // The games just looked at join this run right after the one being read, as what was asked for goes first, and the count says so: it
    // used to say 1 of 1 while they waited for a run of their own.
    {
        sys::Lock lock(_mutex);
        size_t next = std::min(_analysisAt + 1, _analysisQueue.size()), at = next;
        for (auto& key : keys) {
            Row* row = findLocked(key);
            if (!row || row->tab != shown.tab || !unmeasuredLocked(*row) ||
                std::find(_analysisQueue.begin() + (std::ptrdiff_t)next, _analysisQueue.end(), key) != _analysisQueue.end())
                continue;
            _analysisQueue.insert(_analysisQueue.begin() + (std::ptrdiff_t)at++, key);
            shown.count++;
            shown.bytesTotal += row->measureBytes();
        }
    }
    logLine("测量中 " + shown.title + " goes on");
}

// -------------------------------------------------------------------------------------------------------------------
// Making room

/// Refuses early rather than run out of room halfway. The new copy goes beside the old one, wherever that is.
void App::needRoom(Storage storage, i64 output, i64 copy) {
    i64 needed = output + copy + RoomToSpare;
    i64 free = _console ? _console->freeBytes(storage) : sys::freeSpace(_platform.dataFolder());
    if (free >= needed)
        return;
    if (copy > 0)
        throw ToolError(format("处理期间需要 %s 可用空间（%s），其中 %s 用于紧凑 NSZ 的临时副本，当前只有 %s", formatSize(needed).c_str(),
                               storageName(storage), formatSize(copy).c_str(), formatSize(free).c_str()));
    throw ToolError(format("处理期间需要 %s 可用空间（%s），当前只有 %s。先压缩小游戏可以腾出空间", formatSize(needed).c_str(), storageName(storage), formatSize(free).c_str()));
}

void App::needRoomIn(const std::string& folder, i64 output, i64 copy) {
    sys::makeDirectories(folder);
    if (!sys::isDirectory(folder))
        throw ToolError("文件夹 " + folder + " 无法访问。如果在 USB 盘上，请确认已插好。");
    if (output >= sys::largestFile(folder))
        throw ToolError(format("成品大小为 %s，%s 放不下这么大的文件。FAT32 单文件上限 4 GB，需改用 exFAT 或 NTFS", formatSize(output).c_str(), folder.c_str()));
    i64 needed = output + copy + RoomToSpare;
    i64 free = sys::freeSpace(folder);
    if (free >= needed)
        return;
    if (copy > 0)
        throw ToolError(format("需要 %s 可用空间（%s），其中 %s 用于紧凑 NSZ 的临时副本，当前只有 %s", formatSize(needed).c_str(), folder.c_str(),
                               formatSize(copy).c_str(), formatSize(free).c_str()));
    throw ToolError(format("%s 需要 %s 可用空间，当前只有 %s", formatSize(needed).c_str(), folder.c_str(), formatSize(free).c_str()));
}

i64 App::solidMemory() { return std::max<i64>(sys::spareMemory() - (256LL << 20), 0); }

i64 App::temporaryCopy(const Row& row) { return row.solidBytes > 0 ? std::max<i64>(row.solidBytes - solidMemory(), 0) : 0; }

/// Somewhere on the SD card, where a temporary copy goes, rather than on a USB drive.
bool App::onTheSdCard(const std::string& path) {
    if (remote::isRemote(path))
        return false;
    auto drives = _platform.drives();
    return std::none_of(drives.begin(), drives.end(), [&](const Drive& drive) { return drive.isUsb && startsWithNoCase(path, drive.root); });
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
        /// A temporary copy on the SD card while it's worked on, wherever the game goes, which is gone again afterwards.
        i64 copy = 0;
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
        for (const Row* row : visibleRowsLocked(_tab)) {
            // What's left as it is, the question says so, and it takes no room.
            if (!row->ticked || !row->blocked.empty() || !row->ready || !leftAsItIsLocked(*row).empty())
                continue;
            Item item;
            item.key = row->key;
            item.name = row->name;
            // Compressed, what's made can't come to much more than what it's made from, and not compressed, it's already been worked out.
            i64 output = row->hasEstimate ? row->estimate.outputBytes + row->estimate.marginBytes : row->now + row->now / 8;
            item.peak = output + RoomToSpare;
            item.copy = temporaryCopy(*row);
            if (goesOnConsole(*row)) {
                bool toConsole = row->tab == TabInstalled ? row->storage == Storage::Nand : _settings.installToConsole || _consoleOnce;
                item.pool = toConsole ? "Console" : "SD";
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
        if (pool == "Console")
            bytes = _console ? _console->freeBytes(Storage::Nand) : 0;
        else if (pool == "SD")
            bytes = _console ? _console->freeBytes(Storage::Sd) : sys::freeSpace(folder);
        else
            bytes = sys::freeSpace(pool);
        return free[pool] = bytes;
    };

    // How much more it needs, and where: none if it fits.
    auto shortOf = [&](const Item& item, std::string& where) -> i64 {
        where = item.pool;
        i64 missing = item.peak - freeIn(item.pool);
        if (item.copy > 0) {
            i64 onSd = item.copy + (item.pool == "SD" ? item.peak : RoomToSpare) - freeIn("SD");
            if (onSd > missing) {
                missing = onSd;
                where = "SD";
            }
        }
        return missing;
    };

    RoomPlan plan;
    std::vector<bool> done(items.size(), false);
    for (bool progress = true; progress;) {
        progress = false;
        for (size_t i = 0; i < items.size(); i++) {
            if (done[i] || !items[i].tooLarge.empty())
                continue;
            std::string where;
            if (shortOf(items[i], where) <= 0) {
                freeIn(items[i].pool) += items[i].change;
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
        std::string where;
        plan.mostNeeded = std::max(plan.mostNeeded, shortOf(items[i], where));
        plan.countsCopy = plan.countsCopy || (where == "SD" && items[i].copy > 0);
        if (!items[i].tooLarge.empty())
            plan.reason = items[i].tooLarge;
        else if (plan.reason.empty())
            plan.reason = where == "Console" ? "主机" : where == "SD" ? "SD 卡" : "USB 盘";
    }
    return plan;
}

// -------------------------------------------------------------------------------------------------------------------
// How far along it is

std::vector<const char*> App::stepsFor(const Row& row) const {
    std::vector<const char*> steps = stepsOfWorkFor(row);
    // One a first look can't vouch for is read in full first, to know whether it's worth it.
    if (_settings.compress && row.hasEstimate && row.notSure && !row.byHand)
        steps.insert(steps.begin(), StepMeasure);
    return steps;
}

std::vector<const char*> App::stepsOfWorkFor(const Row& row) const {
    // Every new copy is checked: read back once, which it has to be anyway to work out its hash, compared as it is.
    if (goesOnConsole(row)) {
        if (_settings.compress) {
            // What this tool made said so when it was first come across, by what's in it.
            bool allMade = row.updateFile.empty() && row.compressedFile;
            const char* first = row.tab != TabInstalled && (isCompressed(row) || allMade) ? StepCopy : StepCompress;
            return {first, StepCheck, StepFirmware, StepSwap};
        }
        return {StepPrepare, StepWrite, StepCheck, StepFirmware, StepSwap};
    }
    if (_settings.compress)
        return {StepCompress, StepCheckNsp};
    return {StepPrepare, StepWrite};
}

/// What a game comes to in what has to be got through. The sizes of what's on the disk don't say much about that: a game with an update takes up
/// a good deal more than what gets compressed in the end, and what's kept in memory never has to be read back.
void App::workPlannedFor(const Row& row, JobKind kind, i64 (&planned)[2]) const {
    planned[0] = row.now;
    planned[1] = 0;
    // A game's data and the rest of it as they come out, which is all of it unpacked. For a game made one with its update, that can be half
    // of what it takes up on the disk.
    auto unpacked = [](const SizeEstimate& estimate) { return estimate.romFsBytes + std::max<i64>(estimate.outputBytes - estimate.romFsStoredBytes, 0); };
    if (kind == JobKind::Checking || kind == JobKind::Extracting) {
        // Checking reads all of it unpacked, and extracting writes it. A first look leaves the game data of a copy this compressed as it is,
        // so for one of those, it's what the game was before, which the copy says.
        if (row.hasEstimate)
            planned[0] = row.estimate.alreadyCompressed ? row.now + row.estimate.freedBytes : unpacked(row.estimate);
        return;
    }
    if (kind != JobKind::Compressing)
        return;
    i64 output = row.hasEstimate ? row.estimate.outputBytes : row.now;
    i64 gameData = row.hasEstimate ? row.estimate.romFsBytes : row.now;
    if (!_settings.compress) {
        // Its game data checked and its hash tree worked out, and then all of it written, as it comes out.
        planned[0] = gameData + output;
    } else if (row.hasEstimate) {
        // The game data compressed, and the rest of the game written as it is.
        planned[0] = unpacked(row.estimate);
    }
    // A new copy of a game on the console gets all of it read back, to be checked, and an NSP its compressed game data, for its hash.
    if (goesOnConsole(row) || (_settings.compress && gameData > 0))
        planned[1] = output;
    // One a first look can't vouch for is read in full first, as reading a copy back does.
    if (_settings.compress && row.hasEstimate && row.notSure && !row.byHand)
        planned[1] += row.estimate.romFsBytes > 0 ? row.estimate.romFsBytes : row.now;
}

void App::prepareJob(const std::vector<std::string>& keys, JobKind kind) {
    sys::Lock lock(_mutex);
    _results.clear();
    _jobKeys = keys;
    _freeBefore[0] = _freeSd;
    _freeBefore[1] = _freeNand;
    _freeBefore[2] = _freeUsb;
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
        _jobDetail = first->detail;
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
        _jobDetail = row.detail;
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
    static const char* const secondPass[] = {"正在逐字节校验", "正在校验写入内容", "正在为主机准备文件", "正在测量游戏数据"};
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
/// back takes less time than compressing one, so it counts for less. Each game counts for a little on top, for all that isn't either of those,
/// which the game under way earns as its own work goes: with small games that little is much of the bar, and all of it coming at once as a game
/// ends would hold the bar well short of the end until the job is over.
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
    double gamePlanned = (double)_gamePlanned[0] + (double)_gamePlanned[1] * weight;
    if (gamePlanned > 0) {
        double gameDone = (double)(_work[0].done - _gameDoneBefore[0]) + (double)(_work[1].done - _gameDoneBefore[1]) * weight;
        done += std::min(gameDone / gamePlanned, 1.0) * GameOverhead;
    }
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
        {"正在测量游戏数据", StepMeasure},
        {"正在压缩游戏数据", StepCompress},
        {"正在写入", StepCompress},
        {"正在写入", StepCopy},
        {"正在校验写入内容", StepCheckNsp},
        {"正在计算哈希", StepHash},
        {"正在计算哈希", StepCheckNsp},
        {"正在为主机准备文件", StepHash},
        {"正在逐字节校验", StepCheck},
        {"正在获取主机信息", StepFirmware},
        {"正在读取新副本", StepFirmware},
        {"Swapping", StepSwap},
    };
    static const Stage repacking[] = {
        {"正在校验游戏数据", StepPrepare},
        {"正在校验其他部分", StepPrepare},
        {"正在写入", StepWrite},
        {"正在逐字节校验", StepCheck},
        {"正在获取主机信息", StepFirmware},
        {"正在读取新副本", StepFirmware},
        {"Swapping", StepSwap},
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

void App::finishJob(const std::string& title, const std::string& detail, const std::string& advice) {
    logLine(title + ": " + detail + (advice.empty() ? "" : " " + advice));
    sys::Lock lock(_mutex);
    _resultsTitle = title;
    _resultsDetail = detail;
    _resultsAdvice = advice;
    _resultsReady = true;
    _dirty = true;
}

/// What a damaged file system means for whoever's holding the console: the job stopped there, and the card wants looking at on a PC before
/// anything more goes on it. The system can say this of any card, worn out or not, and it's no use going on: what's written next can land
/// on top of something else.
static const char* const DamagedAdvice =
    "系统提示正在写入的文件系统已损坏，因此停止写入。请在电脑上备份并检查 SD 卡，"
    "再用 H2testw 或 F3 检测后再使用。";

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
    if (!game.blocked.empty())
        throw ToolError(game.blocked);
    game.applicationId = application;
    for (auto& meta : game.metas) {
        bool sameTitle = wholeGame ? meta.key.type != Cnmt::MetaAddOnContent : meta.key.id == titleId;
        if (sameTitle && meta.key.version > versionComing)
            throw ToolError(format("已安装更新版本（v%u，此文件为 v%u）", meta.key.version, versionComing));
    }
    return game;
}

/// What a power cut or a crash left half replaced is dealt with when the app opens. If that couldn't be done then, not even checking for it, it's
/// done before anything is replaced, as a new journal would take the place of the only record of it. Throws if it still can't be.
Replacer App::replacer() {
    Replacer replacing(*_console, _platform.dataFolder() + "/journal");
    if (!_journalChecked) {
        for (auto& line : replacing.recover()) {
            logLine(line);
            sys::Lock lock(_mutex);
            _notes.push_back(line + ".");
        }
        _journalChecked = true;
    }
    return replacing;
}

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
            if (const char* test = getenv("HBP_TEST_PROBLEM"); test && std::string(test) == "damaged check")
                throw DamagedFileSystem("无法完成新副本写入（2002-4690）");
            replacer().checkConsole(storage, convertOptions(), _keyset);
            _consoleChecked.insert(storage);
            logLine(format("本主机从 %s 起支持压缩游戏", storageName(storage)));
        } catch (const Cancelled&) {
            finishJob("已停止", "未做任何改动。");
            return false;
        } catch (const ToolError& e) {
            logLine(e.what());
            finishJob("未做任何改动", e.what(), saysFileSystemDamaged(e) ? DamagedAdvice : "");
            return false;
        }
    }
    return true;
}

/// A long job on battery can run the console flat, which cuts the card off in the middle of writing. Before each game it stops while there's
/// still plenty left, saying why. Empty if it can go on.
std::string App::batteryTooLow() {
    constexpr int StopBelow = 15;
    int battery = _platform.batteryPercent();
    if (battery < 0 || battery >= StopBelow || _platform.isCharging())
        return "";
    return format("电量降至 %d%%，已在处理下一个游戏前停止。请接上电源后继续。", battery);
}

void App::compress(const std::vector<std::string>& keys) {
    double started = sys::now();
    // The console in place of the SD card is for this job alone.
    struct ConsoleOnce {
        std::atomic<bool>& flag;
        ~ConsoleOnce() { flag = false; }
    } consoleOnce{_consoleOnce};
    if (!consoleReadsWhatsMade(keys))
        return;

    bool stopped = false, damaged = false;
    std::string lowBattery;
    std::vector<std::string> installed;
    bool savedFile = false;
    for (auto& key : keys) {
        if (_stopAfterGame) {
            stopped = true;
            break;
        }
        lowBattery = batteryTooLow();
        if (!lowBattery.empty()) {
            logLine(lowBattery);
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
            // For the tests, a game that can't be done, with a name and an error code in what it says, as real ones have.
            if (const char* test = getenv("HBP_TEST_PROBLEM"); test && std::string(test) == "job")
                throw ToolError("Couldn't write 'sdmc:/switch/hacBrewPackLZ/temp/game.nca' (2002-0002), for the tests");
            // And one where the system says the card's file system is damaged, as it said to someone whose card was failing.
            if (const char* test = getenv("HBP_TEST_PROBLEM"); test && std::string(test) == "damaged")
                throw ToolError("无法完成新副本写入（2002-4690）");
            compressOne(copy, result, leftOut);
            if (!leftOut && copy.tab != TabInstalled && copy.extracted.empty() && goesOnConsole(copy))
                installed.push_back(copy.key);
        } catch (const Cancelled&) {
            stopped = true;
            state = RowState::Ready;
        } catch (const ToolError& e) {
            state = RowState::Failed;
            // A damaged file system stops the job here: going on to the next game would only write more to it.
            damaged = saysFileSystemDamaged(e);
            result.message = damaged ? std::string(e.what()) + ". " + DamagedAdvice : withNextStep(e.what());
            logLine(copy.name + ": " + result.message);
        } catch (const std::bad_alloc&) {
            state = RowState::Failed;
            result.message = _memoryLimited ? "当前内存不足以处理此游戏。启动任意游戏时按住 R，即可用全部内存打开本程序" : NotEnoughMemory;
            logLine(copy.name + ": " + result.message);
            if (std::string note = memoryNote(); !note.empty())
                logLine(note);
        } catch (const std::exception& e) {
            // Whatever it was, it's this game that couldn't be done, and the rest still get their turn, and their results.
            state = RowState::Failed;
            result.message = withNextStep(std::string(Unexpected) + e.what());
            logLine(copy.name + ": " + result.message);
        }
        if (state == RowState::Failed) {
            problemFound("failed-job", result.message);
            sendWaitingProblems();
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
            // Done now, it isn't remembered as damaged any more.
            if (state == RowState::Done && _cache.erase(cacheKey(*found) + OneFileFailure) > 0)
                saveCacheSoonLocked();
            if (leftOut) {
                judgeLocked(*found, convertOptions());
            } else if (state == RowState::Failed) {
                found->message = result.message;
                // Saved in one file with its DLC, or with the other DLC of a game that isn't there, a file found damaged would only be written
                // and thrown away again each time this opens, so that's remembered. Trouble with the card, the room, memory or a PC may go
                // another way next time.
                std::string plain = plainReason(result.message);
                if (found->tab == TabFiles && !found->dlcFiles.empty() && _settings.dlcInGameFile && !goesOnConsole(copy) &&
                    (plain.rfind("A DLC is damaged", 0) == 0 || plain == "Damaged: dump or download it again.")) {
                    _cache[cacheKey(*found) + OneFileFailure] = RememberedFailure + result.message;
                    saveCacheSoonLocked();
                }
            } else if (state == RowState::Done && copy.tab == TabInstalled && !frees) {
                // An NSP saved of an installed game leaves the game as it was: it isn't compressed, and isn't done. The NSP is then on
                // Game files, compressed and ready to install, which it wouldn't be if this game counted as compressed.
                found->state = RowState::Ready;
                found->ticked = false;
                found->byHand = true;
                found->notes.push_back("其压缩 NSP 已保存到 " + nspFolder() + "，可在「游戏文件」中安装");
            } else if (state == RowState::Done) {
                found->ticked = false;
                found->byHand = false;
                found->belowMinimum.clear();
                // Back to what the game came to before it was ever compressed, for a copy an older version made.
                found->freed = frees ? result.before - result.after + (copy.hasEstimate && copy.estimate.olderCopy ? copy.estimate.freedBytes : 0) : 0;
            }
            // Its game files are judged by how it's installed, which may just have changed: a compressed one is done once the game is
            // installed compressed, and stayed ticked to be installed until the app was opened again.
            if (found->tab == TabInstalled)
                judgeFilesOfLocked(found->application);
        }
        // A game left out wasn't touched, so it freed nothing: counted, it went down as freed in full.
        if (frees && ((state == RowState::Done && !leftOut) || partly))
            _jobFreed += result.before - (partly ? nowBytes : result.after);
        if (!stopped) {
            result.state = leftOut ? RowState::Ready : state;
            if (leftOut)
                result.outcome = "已跳过";
            else if (state == RowState::Done)
                result.outcome = frees || goesOnConsole(copy) ? format("现在 %s", formatSize(result.after).c_str()) : format("已保存为 %s", formatSize(result.after).c_str());
            else
                result.outcome = partly ? "部分完成" : "未改动";
            _results.push_back(result);
            endGameLocked();
        }
        _dirty = true;
        if (stopped || damaged)
            break;
    }

    if (_rescanInstalled.exchange(false))
        _scanned[TabInstalled] = false;
    // What's been saved belongs on the list of game files. Only it needs opening: going through every folder again for it took a while.
    if (savedFile)
        addSavedFiles();
    closeOpenJob();

    std::string detail = compressSummary(sys::now() - started);
    if (!lowBattery.empty())
        detail = lowBattery + " " + detail;
    // Nothing more is deleted from a damaged file system either: what it frees may be in use.
    if (_settings.deleteInstalled && !installed.empty() && !damaged) {
        deleteInstalledFiles(installed, detail);
    } else if (!_settings.deleteAsked && !installed.empty() && !damaged) {
        // Kept, the files are asked about the once, over what came of it.
        sys::Lock lock(_mutex);
        _installedKept = installed;
    }
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
    finishJob(damaged ? "已停止：文件系统损坏" : stopped ? "已停止" : anyFailed ? "已完成，但有问题" : "已完成", detail);
}

/// How it went, in a sentence.
std::string App::compressSummary(double seconds) {
    int finished = 0, failed = 0, left = 0, notWorthIt = 0;
    i64 freed;
    {
        sys::Lock lock(_mutex);
        for (auto& r : _results) {
            if (r.state == RowState::Done) {
                finished++;
            } else if (r.state == RowState::Failed) {
                failed++;
            } else {
                left++;
                notWorthIt += r.notWorthIt ? 1 : 0;
            }
        }
        freed = _jobFreed;
    }
    std::string detail = finished == 1 ? format("1 个游戏已%s", verbed()) : format("%d 个游戏已%s", finished, verbed());
    // Each says why on its own line: below the minimum, or nothing to gain at all.
    if (left > 0)
        detail += notWorthIt == left ? format("，%d 个因收益太小被跳过", left) : format("，%d 个保持原样", left);
    if (failed > 0)
        detail += format("，%d 个处理失败", failed);
    if (freed > 0)
        detail += format("，释放 %s", formatSize(freed).c_str());
    detail += format("，用时 %s。", formatElapsed(seconds).c_str());
    return detail;
}

/// Reads all of a game a first look can't vouch for, writing nothing, and keeps what it comes to, judged again by it. True if that leaves it
/// out, with the reason.
bool App::measureBeforeAnything(const Row& row, ResultLine& result) {
    ConvertOptions options = convertOptions();
    SizeEstimate estimate = measureRow(row);
    logLine(format("已测量 %s：现在 %s，处理后 %s，误差约 %s", row.name.c_str(), formatSize(estimate.inputBytes).c_str(), formatSize(estimate.outputBytes).c_str(),
                   formatSize(estimate.marginBytes).c_str()));
    Row judged = row;
    judged.state = RowState::Ready;
    judged.hasEstimate = true;
    judged.estimate = estimate;
    sys::Lock lock(_mutex);
    if (Row* found = findLocked(row.key)) {
        found->hasEstimate = true;
        found->estimate = estimate;
        found->measureAgain = false;
        rememberEstimateLocked(*found);
        judgeLocked(*found, options);
    }
    judgeLocked(judged, options);
    if (judged.belowMinimum.empty())
        return false;
    result.message = judged.belowMinimum;
    result.notWorthIt = judged.underMinimum;
    return true;
}

void App::compressOne(const Row& row, ResultLine& result, bool& leftOut) {
    ConvertOptions options = convertOptions();
    // What it may keep of a solid NSZ in memory comes first: the new copy holds less of itself in memory, rather than leave that no room.
    if (row.solidBytes > 0)
        options.holdInMemory = std::max<i64>(options.holdInMemory - std::min(row.solidBytes, solidMemory()), 0);
    checkStop();

    // Installing something that's already compressed only takes copying it across. What's installed already, made by an older version, can
    // come out smaller made again.
    if (row.tab != TabInstalled && goesOnConsole(row))
        options.keepCompressed = true;
    if (row.tab == TabInstalled)
        options.improveOlderCopies = true;

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
    // An installed game compressed already by this version, or with no game data to compress, has nothing to gain, whoever ticked it, as
    // on the other versions. One its developer compressed is made again like any other game, ticked by hand, as it's never ticked by itself.
    if (std::string why = options.compress ? nothingToGainReason(row, estimate) : ""; !why.empty()) {
        leftOut = true;
        result.message = why;
        return;
    }
    // Asking for it to be installed is asking for it to be installed, whatever it would gain. One a first look can't vouch for is read in full
    // first, and one ticked by hand is done whatever it saves.
    if (!row.byHand && !_installNow) {
        Row judged = row;
        // As it was on the list. Marked as being worked on, a compressed game file didn't count as compressed, and so not as one to install:
        // every ticked one was left out as already compressed.
        judged.state = RowState::Ready;
        judged.hasEstimate = true;
        judged.estimate = estimate;
        {
            sys::Lock lock(_mutex);
            judgeLocked(judged, options);
        }
        // One a first look can't vouch for is read in full first, writing nothing, and done if that finds it's worth it. One that reading in
        // full can need a temporary copy on the SD card for goes by its first look, rather than have that copy written twice.
        if (judged.notSure && temporaryCopy(row) == 0) {
            if (measureBeforeAnything(row, result)) {
                leftOut = true;
                return;
            }
            sys::Lock lock(_mutex);
            if (Row* found = findLocked(row.key))
                estimate = found->estimate;
        } else if (!judged.notSure && !judged.belowMinimum.empty()) {
            leftOut = true;
            result.message = judged.belowMinimum;
            result.notWorthIt = judged.underMinimum;
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

/// A little over what a file is expected to come to, which decides what kind of file it gets: on a FAT32 card, one of 4 GB or more has to be
/// split. The estimate is for the whole row, a game with its update and DLC, so a file gets the share of it of the titles that go into it, one
/// or several, and never less than they come to now, as a DLC can compress less than the game does.
static i64 sizeHintFor(const SizeEstimate& estimate, i64 titleBytes) {
    i64 expected = estimate.outputBytes + estimate.marginBytes;
    if (estimate.inputBytes > 0 && titleBytes > 0)
        expected = std::max(titleBytes, (i64)((double)titleBytes * (double)expected / (double)estimate.inputBytes));
    return expected + expected / 16;
}

/// Where a file the app saves goes: the name given if nothing's there, or else the same with " (2)", " (3)" and on before ".nsp", as the PC
/// versions do. What's there already is somebody's, and may well be what this is being made from, such as an original game named the way this
/// names what it makes. A card that won't say whether a name is free stops it there, rather than anything being written over.
static std::string freeOutputPath(const std::string& folder, const std::string& fileName) {
    constexpr int MostCopies = 999;
    size_t dot = fileName.rfind('.');
    std::string stem = dot == std::string::npos ? fileName : fileName.substr(0, dot);
    std::string extension = dot == std::string::npos ? std::string() : fileName.substr(dot);
    for (int copy = 1; copy <= MostCopies; copy++) {
        std::string path = joinPath(folder, copy == 1 ? fileName : format("%s (%d)%s", stem.c_str(), copy, extension.c_str()));
        switch (sys::presence(path)) {
            case sys::Presence::Missing:
                return path;
            case sys::Presence::There:
                break;
            case sys::Presence::CantTell:
                throw ToolError("无法确认「" + path + "」是否已存在，因此未覆盖保存，请稍后重试");
        }
    }
    throw ToolError(format("%s 中已有 %d 个名为「%s」的文件（带序号）。请删除部分文件，或另选 NSP 文件夹", folder.c_str(), MostCopies, stem.c_str()));
}

void App::compressInstalled(const Row& row, const SizeEstimate& estimate, const ConvertOptions& options, ResultLine& result) {
    if (!replacesInstalled()) {
        needRoomIn(nspFolder(), estimate.outputBytes + estimate.marginBytes);
        auto loaded = loadGame(row.application);
        std::vector<const TitleJob*> jobs;
        for (auto& job : loaded->set->jobs)
            jobs.push_back(&job);
        // All of an installed game is saved, whatever of it is compressed already, as before.
        saveNsps(jobs, *loaded->set, estimate, options, false, false, {}, result);
        return;
    }

    needRoom(row.storage, estimate.outputBytes);
    // A title at a time, reading the console afresh each time, as the last one will have changed what's there.
    Replacer replacing = replacer();
    std::set<u64> done;
    while (true) {
        auto loaded = loadGame(row.application);
        // The list may have been put together before it was known which game this app runs through, or which is open.
        if (!loaded->game.blocked.empty())
            throw ToolError(loaded->game.blocked);
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
        // A DLC compressing wouldn't change, as it's only a few bytes or compressed already, stays as it is rather than be swapped for the same.
        if (!job->isBaseGame() && isAlreadyCompressed(*job, options))
            continue;
        replacing.run(loaded->game, *job, options, _keyset);
    }
    InstalledGame now = _console->describe(row.application);
    result.after = now.totalBytes();

    // It's now what it is, and there's no need to go looking on the next start to find that out.
    sys::Lock lock(_mutex);
    if (Row* found = findLocked(row.key)) {
        applyContents(*found, now);
        found->estimate = SizeEstimate{result.after, result.after, 0, 0, 0, options.compress};
        // What compressing it has freed, which the copy goes on saying for as long as it's installed: what this freed, and for a copy an older
        // version made, what that had freed already, so it goes back to what the game came to before it was ever compressed.
        i64 before = estimate.olderCopy ? estimate.freedBytes : 0;
        found->estimate.freedBytes = options.compress ? std::max<i64>(result.before - result.after + before, 0) : 0;
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
    // Beside where it was extracted from, the game it came from may well be, under this very name.
    std::string path = freeOutputPath(folder, game.outputFileName(options));
    NspOutput nsp(path, game.contentCount(), sizeHintFor(estimate, game.inputBytes));
    TitleConverter(options, _keyset).build(game, nsp);
    result.after = nsp.outputBytes();
    logLine("已保存 " + path);
    noteSaved(path);
}

/// The game first and then each of its DLC.
void App::compressFiles(const Row& row, const SizeEstimate& estimate, const ConvertOptions& options, ResultLine& result) {
    OpenedJobs opened = openJobs(row);
    bool installing = goesOnConsole(row);
    std::string folder = nspFolder();
    // A temporary copy of a solid NSZ goes on the SD card, beside the new copy if that's where it goes too.
    i64 copy = temporaryCopy(row);
    bool besideIt = installing ? installTarget() == Storage::Sd : onTheSdCard(folder);
    if (installing)
        needRoom(installTarget(), estimate.outputBytes, besideIt ? copy : 0);
    else
        needRoomIn(folder, estimate.outputBytes + estimate.marginBytes, besideIt ? copy : 0);
    if (copy > 0 && !besideIt)
        needRoom(Storage::Sd, 0, copy);
    if (!installing) {
        saveNsps(opened.jobs, *opened.set, estimate, options, true, row.dlcNotInFile, row.dlcWithoutFile, result);
        return;
    }
    for (const TitleJob* job : opened.jobs) {
        checkStop();
        u32 version = job->update ? job->update->meta.version : job->main->meta.version;
        InstalledGame game = whatsInstalled(row.application, version, job->isBaseGame(), job->main->meta.titleId);
        result.after += replacer().run(game, *job, options, _keyset, installTarget()).after;
        _rescanInstalled = true;
    }
}

/// A game and its DLC saved as NSPs, as on the PC. With Game and DLC in one file on, the game's new file holds its DLC too, after it in title ID
/// order, and DLC without their game go together into one file, named after the first. Off, each DLC gets a file of its own. Once the game's
/// file is made, none of its DLC is left out, apart from one this tool compressed before with the setting off, which is a file of its own
/// already. For game files (leaveCompressed), what's already compressed otherwise stays where it is, as it is, unless their file is made again
/// to hold all of them (remake), or it's a DLC with no file of its own (needFile), and a file of DLC without their game holds all of them once
/// any of them is made.
void App::saveNsps(const std::vector<const TitleJob*>& jobs, const TitleSet& set, const SizeEstimate& estimate, const ConvertOptions& options, bool leaveCompressed,
                   bool remake, const std::vector<u64>& needFile, ResultLine& result) {
    auto gameAt = std::find_if(jobs.begin(), jobs.end(), [](const TitleJob* job) { return job->isBaseGame(); });
    const TitleJob* game = gameAt == jobs.end() ? nullptr : *gameAt;
    auto asItWas = [&](const TitleJob* job) {
        return leaveCompressed && !remake && isAlreadyCompressed(*job, options) &&
               std::find(needFile.begin(), needFile.end(), job->main->meta.titleId) == needFile.end();
    };
    bool gameMade = game && !asItWas(game);
    // v23：整合打包按 ApplicationId 全量收集 DLC，与 PC 端自建 RunMerged 一致。
    // 上游 1.5.1 只把本行按文件枚举顺序列出的 DLC 附到本体后面；拖入文件夹时 DLC 分散在
    // 别的文件或顺序在前，就会漏掉 DLC，使 CNMT 数量与 NSP 预留文件数不符，成品实机报错。
    u64 application = game ? game->main->meta.titleId
                    : jobs.empty() ? 0 : jobs.front()->isBaseGame() ? jobs.front()->main->meta.titleId : jobs.front()->main->meta.linkedId();
    std::vector<const TitleJob*> dlc;
    if (options.dlcInGameFile && application) {
        for (auto& candidate : set.jobs) {
            const TitleJob* job = &candidate;
            if (job->isBaseGame() || job->main->meta.linkedId() != application)
                continue;
            if (std::find(dlc.begin(), dlc.end(), job) == dlc.end())
                dlc.push_back(job);
        }
    }
    for (const TitleJob* job : jobs) {
        if (job == game || std::find(dlc.begin(), dlc.end(), job) != dlc.end())
            continue;
        // 只并入本行的 ApplicationId，文件夹里混入其它游戏的 DLC 时不会写进错误的文件。
        if (application && !job->isBaseGame() && job->main->meta.linkedId() != application)
            continue;
        dlc.push_back(job);
    }
    std::sort(dlc.begin(), dlc.end(), [](const TitleJob* a, const TitleJob* b) { return a->main->meta.titleId < b->main->meta.titleId; });
    if (remake && game)
        logLine("其 DLC 未包含在文件内，将重新生成含该 DLC 的游戏文件");
    // Each file to make, with the titles that go into it. The game's file, or with no game, one file of its DLC.
    bool together = options.dlcInGameFile && (gameMade || !game);
    bool anyMade = gameMade || std::any_of(dlc.begin(), dlc.end(), [&](const TitleJob* job) { return !asItWas(job); });
    std::vector<std::vector<const TitleJob*>> files;
    if (gameMade)
        files.push_back({game});
    else if (game)
        result.after += game->inputBytes;
    for (const TitleJob* job : dlc) {
        bool asItIs = gameMade ? leaveCompressed && options.compress && !options.dlcInGameFile && isMadeByThisTool(*job) : together ? !anyMade : asItWas(job);
        if (asItIs) {
            result.after += job->inputBytes;
        } else if (together) {
            if (files.empty())
                files.emplace_back();
            files[0].push_back(job);
        } else {
            files.push_back({job});
        }
    }

    std::string folder = nspFolder();
    for (auto& titles : files) {
        checkStop();
        int contents = 0;
        i64 bytes = 0;
        for (const TitleJob* title : titles) {
            contents += (int)title->contents.size();
            bytes += title->inputBytes;
        }
        std::string path = freeOutputPath(folder, outputFileName(*titles[0], set, options));
        NspOutput nsp(path, contents, sizeHintFor(estimate, bytes), false, (int)titles.size());
        // v23：每个标题各用独立的 TitleConverter，与 PC 端 RunMerged 一致。更新映射、副本标记
        // 与 NCA 写出状态都不跨标题复用，避免 DLC 带上本体的内容记录或压缩表。
        for (size_t i = 0; i < titles.size(); i++) {
            try {
                TitleConverter converter(options, _keyset);
                converter.run(*titles[i], nsp);
            } catch (const DamagedFileSystem&) {
                throw;
            } catch (const ToolError& e) {
                if (i == 0)
                    throw;
                // The game's file fails with it, so the message names the DLC, and for game files the file it's in.
                std::string which = format("其 DLC %016llX", (unsigned long long)titles[i]->main->meta.titleId);
                if (leaveCompressed)
                    which += " in '" + fileNameOf(titles[i]->main->archive->path()) + "'";
                throw ToolError(which + ": " + e.what());
            }
        }
        result.after += nsp.outputBytes();
        logLine("已保存 " + path);
        noteSaved(path);
    }
}

/// The files a job installed everything in, which can go: one with anything in it not installed yet stays.
std::set<std::string> App::installedFilesLocked(const std::vector<std::string>& installed, bool say) {
    std::set<std::string> paths;
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
        else if (say)
            logLine("已保留 " + path + "：其中其他游戏尚未安装");
    }
    return paths;
}

/// A file goes once everything in it has gone onto the console, and not before.
void App::deleteInstalledFiles(const std::vector<std::string>& installed, std::string& detail) {
    // A file that's still open can't be deleted.
    closeOpenJob();

    std::set<std::string> paths;
    {
        sys::Lock lock(_mutex);
        paths = installedFilesLocked(installed, true);
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
            logLine("已删除 " + path);
        } else {
            logLine("无法删除 " + path);
        }
    }
    sys::commit(nspFolder());
    if (deleted > 0)
        detail += deleted == 1 ? " 已删除 1 个文件。" : format(" 已删除 %d 个文件。", deleted);
}

/// Asked the once, over what came of the first job that installed files and kept them: deleting them frees what they take, and it can be
/// done by itself from then on. Files on a PC aren't this app's to delete.
void App::offerToDeleteInstalled() {
    std::vector<std::string> installed;
    std::set<std::string> paths;
    {
        sys::Lock lock(_mutex);
        installed.swap(_installedKept);
        if (installed.empty() || _settings.deleteInstalled || _settings.deleteAsked)
            return;
        for (auto& path : installedFilesLocked(installed, false)) {
            if (!remote::isRemote(path))
                paths.insert(path);
        }
    }
    _settings.deleteAsked = true;
    saveSettings();
    i64 bytes = 0;
    for (auto& path : paths)
        bytes += std::max<i64>(sys::fileSize(path), 0);
    if (paths.empty() || bytes <= 0)
        return;
    bool one = paths.size() == 1;
    auto dialog = std::make_unique<Dialog>();
    dialog->title = one ? "删除已安装的文件？" : format("删除这 %d 个已安装文件？", (int)paths.size());
    dialog->paragraphs.push_back(format("%s %s，现已安装 %s，删除%s可释放这部分空间。", one ? "It takes" : "They take", formatSize(bytes).c_str(),
                                        one ? "对应游戏为" : "对应游戏为", one ? "it" : "them"));
    dialog->paragraphs.push_back("文件安装完成后，可在设置中让它自动执行。");
    auto remove = [this, installed](bool always) {
        if (always) {
            _settings.deleteInstalled = true;
            saveSettings();
        }
        std::string detail;
        {
            sys::Lock lock(_mutex);
            detail = _resultsDetail;
        }
        deleteInstalledFiles(installed, detail);
        sys::Lock lock(_mutex);
        _resultsDetail = detail;
        _dirty = true;
    };
    dialog->choices.push_back({one ? "删除文件" : "删除", [remove] { remove(false); }});
    dialog->choices.push_back({"始终删除", [remove] { remove(true); }});
    dialog->choices.push_back({one ? "保留文件" : "保留它们", nullptr});
    dialog->cancelChoice = 2;
    // It comes up over what came of it, where A goes back to the list, so A out of habit keeps them.
    dialog->focus = 2;
    _dialog = std::move(dialog);
    _dirty = true;
}

// -------------------------------------------------------------------------------------------------------------------
// Extracting, checking and removing

App::ResultLine App::resultFor(const Row& row) {
    ResultLine result;
    result.key = row.key;
    result.name = row.name;
    result.before = row.now;
    result.icon = row.icon;
    result.tile = row.tile;
    return result;
}

/// Each game in turn, for what doesn't need anything more than doing something to it and saying how that went.
void App::workThrough(const std::vector<std::string>& keys, const char* whenItFails, const std::function<void(const Row&, ResultLine&)>& work, const std::function<std::string(int, double)>& summary) {
    double started = sys::now();
    bool stopped = false, damaged = false;
    int failed = 0;
    std::string lowBattery;
    for (auto& key : keys) {
        if (_stopAfterGame || damaged) {
            stopped = true;
            break;
        }
        lowBattery = batteryTooLow();
        if (!lowBattery.empty()) {
            logLine(lowBattery);
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
            // A damaged file system stops it here, as going on would only write more to it.
            damaged = saysFileSystemDamaged(e);
            fail(damaged ? std::string(e.what()) + ". " + DamagedAdvice : std::string(e.what()));
        } catch (const std::bad_alloc&) {
            if (std::string note = memoryNote(); !note.empty())
                logLine(note);
            fail(NotEnoughMemory);
        } catch (const std::exception& e) {
            fail(std::string(Unexpected) + e.what());
        }
        if (stopped)
            break;
        sys::Lock lock(_mutex);
        _results.push_back(result);
        endGameLocked();
    }
    std::string detail = summary(failed, sys::now() - started);
    finishJob(damaged ? "已停止：文件系统损坏" : stopped ? "已停止" : failed > 0 ? "已完成，但有问题" : "已完成",
              lowBattery.empty() ? detail : lowBattery + " " + detail);
}

/// What a game's work is done on: the titles of an installed game, or of the files of a game file's row with its DLC.
void App::forEachTitle(const Row& row, const std::function<void(const TitleJob&, const TitleSet&)>& work) {
    if (row.tab == TabInstalled) {
        auto loaded = loadGame(row.application);
        for (auto& job : loaded->set->jobs) {
            // It's gone through as it will be once its update is made part of it, which takes the update fitting it, rather than
            // seeming damaged when it doesn't.
            if (loaded->game.updateOverCopy)
                checkUpdateFits(job, true);
            work(job, *loaded->set);
        }
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
            throw ToolError("文件夹 " + output + " 无法访问。如果在 USB 盘上，请确认已插好。");
        i64 free = sys::freeSpace(output);
        if (free < row.now + RoomToSpare)
            throw ToolError(format("至少需要 %s 可用空间（%s），当前只有 %s", formatSize(row.now + RoomToSpare).c_str(), output.c_str(), formatSize(free).c_str()));
        i64 bytes = 0;
        forEachTitle(row, [&](const TitleJob& job, const TitleSet& set) {
            // There's no use in doing it again over what's there, and half a game left behind would look like a whole one on the list.
            std::string folder = extractedFolderOf(job, set, output);
            if (ExtractedGame::isOne(folder))
                throw ToolError("它已位于 " + folder + "。若要重新解包，请先从「游戏文件」删除它；若已完成，可在该列表的「已完成」中找到");
            // Whatever's left of a folder that isn't a whole game is no use to anyone.
            sys::removeTree(folder);
            try {
                bytes += extractTitle(job, set, output).bytes;
            } catch (...) {
                sys::removeTree(folder);
                throw;
            }
        });
        result.outcome = format("已解包 %s", formatSize(bytes).c_str());
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
        std::string what = failed == 0 ? "所有游戏回读校验通过" : failed == 1 ? "1 个游戏回读校验未通过" : format("%d 个游戏回读校验未通过", failed);
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
    }, [&](int failed, double seconds) { return failed == 0 ? format("用时 %s。", formatElapsed(seconds).c_str()) : failed == 1 ? std::string("其中 1 个保持原样。") : format("其中 %d 个保持原样。", failed); });

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
        std::set<u64> uninstalled;
        for (auto& r : _rows) {
            if (r.tab == TabInstalled && std::find(removed.begin(), removed.end(), r.key) != removed.end())
                uninstalled.insert(r.application);
        }
        _rows.erase(std::remove_if(_rows.begin(), _rows.end(), [&](const Row& r) { return std::find(removed.begin(), removed.end(), r.key) != removed.end(); }), _rows.end());
        if (!uninstalled.empty())
            installedGoneLocked(uninstalled);
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

/// What the console goes by is its record of a game, not what the game's own files say, so that's all that changes. A power cut leaves the
/// record as it was or as it's meant to be, as the console writes it whole.
void App::fixMarks(const std::vector<std::string>& keys) {
    std::vector<std::string> looked;
    workThrough(keys, "Not fixed", [&](const Row& row, ResultLine& result) {
        sink().progress("Fixing it", 0, 1, 0);
        InstalledGame game = _console->describe(row.application);
        if (!game.blocked.empty())
            throw ToolError(game.blocked);
        looked.push_back(row.key);
        judgeMark(game);
        // Fixed already, or changed since: there's nothing it says that it shouldn't.
        if (!game.markedIncomplete) {
            result.outcome = "无需修复";
            return;
        }
        const InstalledMeta* base = nullptr;
        for (auto& meta : game.metas)
            base = meta.key.type == Cnmt::MetaApplication ? &meta : base;
        Bytes record = _console->getMeta(base->storage, base->key);
        if (record.size() < 8)
            throw ToolError("其记录格式与主机不符，因此保持原样");
        std::vector<std::string> fixed;
        if (record[6] & Cnmt::AttributeCompacted) {
            record[6] &= (u8)~Cnmt::AttributeCompacted;
            fixed.push_back("已不再标记为需要更新补丁");
        }
        // What the installer it came from had it ask for is gone with the old record, so it asks for this console's own version, which is all
        // it needs to start here, and never less than compressed games need.
        if (game.asksSystem) {
            if (record.size() < 8 + 0xC || rd16(record.data()) < 0xC)
                throw ToolError("其记录中系统版本要求的位置与主机格式不符，因此保持原样");
            u32 system = std::max(_platform.systemVersion(), CompressionMinSystemVersion);
            wr32(record.data() + 8 + 8, system);
            fixed.push_back(format("要求系统版本 %s，而非 %s", versionName(system).c_str(), versionName(game.asksSystem).c_str()));
        }
        _console->setMeta(base->storage, base->key, record);
        // The version installed is the one it has to be happy with, as after compressing. The game starts without it, so a no isn't a failure.
        try {
            _console->setLaunchVersion(row.application, base->key.version);
        } catch (const ToolError& e) {
            logLine(row.name + "：无法告知主机启动版本：" + e.what());
        }
        result.outcome = "已修复";
        logLine("Fixed " + row.name + "：其记录 " + joined(fixed, ", and "));
    }, [&](int failed, double seconds) { return failed == 0 ? format("用时 %s。", formatElapsed(seconds).c_str()) : failed == 1 ? std::string("其中 1 个修复失败。") : format("其中 %d 个修复失败。", failed); });

    // What the list says goes by what's there now.
    for (auto& key : looked) {
        u64 application = 0;
        {
            sys::Lock lock(_mutex);
            Row* found = findLocked(key);
            if (!found)
                continue;
            application = found->application;
        }
        try {
            InstalledGame now = _console->describe(application);
            judgeUpdateOver(now);
            judgeMark(now);
            sys::Lock lock(_mutex);
            if (Row* found = findLocked(key)) {
                applyContents(*found, now);
                _listChanged = true;
                _dirty = true;
            }
        } catch (const ToolError& e) {
            logLine(format("%016llx can't be read: %s", (unsigned long long)application, e.what()));
        }
    }
}

/// The folder of an extracted game, a file at a time, as with thousands of them, all at once would look like it had got stuck.
void App::deleteFolder(const std::string& path) {
    // Whatever the list says, this only ever goes for what's plainly a game: a question the system won't answer doesn't count as a yes.
    if (sys::presence(path + "/exefs/main.npdm") != sys::Presence::There || sys::presence(path + "/control/control.nacp") != sys::Presence::There)
        throw ToolError("此文件夹已不像游戏，或系统暂时无法确认，因此未做处理");
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
