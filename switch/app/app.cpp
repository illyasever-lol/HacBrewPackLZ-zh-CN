// Starting up and closing down, the log, the threads the work runs on, and updating the app itself.
#include "app_internal.h"

#include <cstdio>

#include "../core/remote.h"

namespace hbp {

using namespace app;

namespace {

constexpr const char* ReleasesUrl = "https://gitlab.com/api/v4/projects/ismaellozanoperezserrano%2Fhacbrewpacklz/releases?per_page=1";
constexpr const char* DownloadsUrl = "https://gitlab.com/ismaellozanoperezserrano/hacbrewpacklz/-/releases/";
constexpr const char* UpdateManifestName = "hacbrewpacklz-update.txt";
constexpr const char* UpdateSignatureName = "hacbrewpacklz-update.sig";
constexpr size_t LargestProgram = 64 << 20;

/// Whether the first version comes after the second, going by up to three numbers.
bool isNewerVersion(const std::string& a, const std::string& b) {
    int x[3] = {0, 0, 0}, y[3] = {0, 0, 0};
    sscanf(a.c_str(), "%d.%d.%d", &x[0], &x[1], &x[2]);
    sscanf(b.c_str(), "%d.%d.%d", &y[0], &y[1], &y[2]);
    for (int i = 0; i < 3; i++) {
        if (x[i] != y[i])
            return x[i] > y[i];
    }
    return false;
}

}  // namespace

/// How the job on the worker thread tells the screen how it's getting on.
class App::Sink : public JobSink {
public:
    explicit Sink(App& app) : _app(app) {}

    void message(const std::string& text) override { _app.logLine(text); }
    void warning(const std::string& text) override { _app.logLine("警告：" + text); }

    void counting(const char* label, i64 sofar) override {
        bool changed;
        {
            sys::Lock lock(_app._mutex);
            changed = _app._jobLabel != label;
            _app._jobLabel = label;
            _app._jobDone = _app._jobTotal = 0;
            _app._jobFilesDone = sofar;
            _app._jobFilesTotal = 0;
        }
        if (changed)
            _app.logLine(label);
    }

    void files(i64 done, i64 total) override {
        sys::Lock lock(_app._mutex);
        _app._jobFilesDone = done;
        _app._jobFilesTotal = total;
    }

    void progress(const char* label, i64 done, i64 total, i64) override {
        bool changed;
        {
            sys::Lock lock(_app._mutex);
            changed = _app._jobLabel != label;
            double now = sys::now();
            if (changed) {
                _app._jobLabel = label;
                _app._jobRate = 0;
                _app._jobRateDone = done;
                _app._jobRateAt = now;
            } else if (now - _app._jobRateAt >= 1) {
                double rate = (double)(done - _app._jobRateDone) / (now - _app._jobRateAt);
                _app._jobRate = _app._jobRate > 0 ? _app._jobRate * 0.7 + rate * 0.3 : rate;
                _app._jobRateDone = done;
                _app._jobRateAt = now;
            }
            _app._jobDone = done;
            _app._jobTotal = total;
            _app.noteStepLocked(label);
            _app.noteWorkLocked(label, done);
        }
        if (changed)
            _app.logLine(label);
    }

private:
    App& _app;
};

App::App(Platform& platform) : _platform(platform) {
    _sink.reset(new Sink(*this));
    setSink(_sink.get());
}

App::~App() {
    shutdown();
    setSink(nullptr);
}

void App::shutdown() {
    _sink->stop = true;
    _worker.join();
    _analyst.join();
    _updateStop = true;
    _updateThread.join();
    {
        sys::Lock lock(_mutex);
        if (_cacheChanged)
            saveCacheLocked();
    }
    saveFileIndex();
    if (_working) {
        _working = false;
        _platform.setWorking(false, false);
    }
    if (!_closedDown && _log) {
        _closedDown = true;
        if (sys::outOfHandlesCount() > 0)
            logLine(format("Asked again %lld times after being told it was out of handles", (long long)sys::outOfHandlesCount()));
        logLine("正在关闭");
        logFolder(_platform.dataFolder());
    }
}

// -------------------------------------------------------------------------------------------------------------------
// The log

void App::logLine(const std::string& text) {
    sys::Lock lock(_logMutex);
    std::string line = format("[%7.1f] %s\n", sys::now(), text.c_str());
    if (!_logDecided) {
        _earlyLog.push_back(line);
        return;
    }
    if (!_log)
        return;
    try {
        _log->write(_logPosition, line.data(), line.size());
        _logPosition += (i64)line.size();
        _log->flush();
        // If the console goes down, what's in a file that's still open can go with it, and the file too. Going through the lists is where that's been seen.
        double now = sys::now();
        if (_jobKind == JobKind::Scanning || _jobKind == JobKind::Extracting || now - _logClosedAt >= 1) {
            _logClosedAt = now;
            _log.reset();
            sys::commit(_logPath);
            _log.reset(new sys::File(_logPath, sys::File::Mode::Update));
        }
    } catch (const ToolError&) {
        // A log that can't be written isn't worth stopping for.
    }
}

/// There's only ever the one log, of this time. Earlier versions kept more, which go.
void App::removeOldLogs() {
    std::string folder = _platform.dataFolder();
    for (const char* old : {"/log.1.txt", "/log.2.txt", "/log-other.txt", "/failure.txt"})
        sys::removeFile(folder + old);
}

void App::openLog() {
    try {
        removeOldLogs();
        _logPath = _platform.dataFolder() + "/log.txt";
        try {
            _log.reset(new sys::File(_logPath, sys::File::Mode::CreateOnce));
        } catch (const ToolError& e) {
            // There's been a card where this one name can't be created, which is no reason to go without.
            _logProblem = e.what();
            _logPath = _platform.dataFolder() + "/log-other.txt";
            _log.reset(new sys::File(_logPath, sys::File::Mode::Create));
            logLine(_logProblem);
            _logProblem.clear();
        }
    } catch (const ToolError& e) {
        _logProblem = e.what();
        _log.reset();
    }
}

/// What's there, as it's the only way of telling afterwards what became of something that was saved.
void App::logFolder(const std::string& folder) {
    std::string files;
    for (auto& entry : sys::list(folder)) {
        if (!entry.isDirectory)
            files += format("%s%s %lld", files.empty() ? "" : ", ", entry.name.c_str(), (long long)entry.size);
    }
    logLine("In " + folder + ": " + (files.empty() ? std::string("nothing") : files));
}

void App::noteUnexpected(const std::string& what) {
    logLine("意外失败：" + what);
    sys::Lock lock(_mutex);
    _unexpected = what;
}

// -------------------------------------------------------------------------------------------------------------------
// Starting up

void App::loadKeys() {
    _keysPath.clear();
    _keysProblem.clear();
    for (auto& path : _platform.keyLocations()) {
        if (!sys::exists(path))
            continue;
        try {
            _keyset.load(path);
            _keyset.headerKey();
            if (!_keyset.keyAreaKey(0, 0))
                throw ToolError("其中缺少 key_area_key_application_00");
            _keysPath = path;
            logLine("密钥来自" + path);
            return;
        } catch (const ToolError& e) {
            _keysProblem = path + ": " + e.what();
        }
    }
    logLine(_keysProblem.empty() ? "未找到密钥" : _keysProblem);
}

void App::start() {
    try {
        sys::makeDirectories(_platform.dataFolder());
        sys::makeDirectories(_platform.backupFolder());
    } catch (const ToolError&) {
    }
    loadSettings();
    if (_settings.writeLog)
        openLog();
    {
        sys::Lock lock(_logMutex);
        _logDecided = true;
    }
    for (auto& line : std::vector<std::string>(std::move(_earlyLog)))
        logLine(line.substr(line.find(']') + 2, line.size() - line.find(']') - 3));
    logLine(format("hacBrewPackLZ for Switch %s, built %s %s", AppVersion, __DATE__, __TIME__));
    logLine(_platform.aboutSystem());
    logFolder(_platform.dataFolder());
    logFolder(_platform.backupFolder());

    _platform.loadFonts(_font);
    _dark = _platform.prefersDark();
    _memoryLimited = _platform.memoryIsLimited();
    loadCache();
    loadHidden();
    buildOptions();
    buildWelcome();
    loadKeys();
    _drives = _platform.drives();

    _console = _platform.console();
    if (!_console) {
        logLine("主机服务不可用：" + _platform.consoleProblem());
        _tab = TabFiles;
        _tabShown = TabFiles;
        _scanned[TabInstalled] = true;
    }
    if (_keysPath.empty()) {
        _screen = Screen::NoKeys;
        return;
    }
    // With the app's own folder gone, it's being set up afresh, even if the settings can be had from their other place.
    if (!sys::exists(_platform.dataFolder() + "/settings.txt") && !sys::exists(_platform.dataFolder() + "/settings.txt.new"))
        _settings.welcomed = false;
    if (!_settings.welcomed)
        _screen = Screen::Welcome;
    loadList();
    loadFileIndex();
    logLine(format("Remembered from last time: %d answers, %d games on the lists", (int)_cache.size(), (int)_rows.size()));
    if (_settings.autoUpdate)
        startUpdateCheck(false);
    runInBackground(JobKind::Starting, [this] { startingUp(); });
}

/// The keys weren't there when this opened, and may be now.
void App::lookForKeysAgain() {
    loadKeys();
    if (!_keysPath.empty()) {
        _screen = _settings.welcomed ? Screen::Main : Screen::Welcome;
        loadList();
        loadFileIndex();
        runInBackground(JobKind::Starting, [this] { startingUp(); });
    }
    _dirty = true;
}

void App::startingUp() {
    // Whatever a power cut left behind goes first, before anything else looks at the console.
    sys::removeTree(_platform.dataFolder() + "/temp");
    if (!_console)
        return;
    try {
        Replacer replacer(*_console, _platform.dataFolder() + "/journal");
        for (auto& line : replacer.recover()) {
            logLine(line);
            sys::Lock lock(_mutex);
            _notes.push_back(line + ".");
        }
    } catch (const ToolError& e) {
        logLine(std::string("无法清理上次的遗留内容：") + e.what());
        sys::Lock lock(_mutex);
        _notes.push_back(std::string("上次任务被中断，且清理未完成，下次启动会重试。") + e.what());
    }
}

// -------------------------------------------------------------------------------------------------------------------
// The threads: one for jobs and looking through the lists, and one beside it for analyzing

void App::runInBackground(JobKind kind, std::function<void()> work) {
    _worker.join();
    // A list coming up can go on beside analyzing, unless everything's been asked to stop.
    if (kind != JobKind::Scanning || _sink->stop)
        stopAnalyst();
    {
        sys::Lock lock(_mutex);
        _jobLabel.clear();
        _jobDone = _jobTotal = 0;
        _jobStarted = sys::now();
        _unexpected.clear();
    }
    _sink->stop = false;
    _scanStop = false;
    _stopAfterGame = false;
    _jobKind = kind;
    _workerRunning = true;
    _dirty = true;
    _worker.start(
        [this, kind, work = std::move(work)] {
            // Waiting on a PC gives way when asked, as anything else this does: a list, when it's to give way to what's been asked for.
            remote::setGiveWay([this, kind] { return _sink->stop.load() || (kind == JobKind::Scanning && _scanStop.load()); });
            try {
                work();
            } catch (const Cancelled&) {
                logLine("Stopped");
                sys::Lock lock(_mutex);
                _unexpected = "任务在得出结果前就被停止了";
            } catch (const std::exception& e) {
                noteUnexpected(e.what());
            } catch (...) {
                noteUnexpected("非普通错误");
            }
            _scanningTab = -1;
            _jobKind = JobKind::None;
            _workerRunning = false;
            _dirty = true;
        },
        -1, 1 << 21);
}

/// Looking through a list or analyzing it can always be picked up again, and has to stop before what it's working from can change.
void App::quietenWorker() {
    if (_jobKind == JobKind::Scanning) {
        _sink->stop = true;
        _worker.join();
    }
    stopAnalyst();
}

void App::stopAnalyst() {
    if (_analystRunning)
        _sink->stop = true;
    _analyst.join();
}

/// What a list scan checks for, which gives way to the list on the screen as well as to being asked to stop.
void App::checkScanStop() {
    if (_scanStop)
        throw Cancelled();
    checkStop();
}

void App::startAnalyzing() {
    std::vector<std::string> keys;
    int tab = -1;
    {
        sys::Lock lock(_mutex);
        // The list on the screen first, and then the others that have come up, so that it's not left waiting for anyone to go and look at them.
        if (_autoAnalyze[_tab])
            keys = keysToAnalyzeLocked(_tab);
        if (!keys.empty())
            tab = _tab;
        for (int other = 0; other < TabSettings && keys.empty(); other++) {
            if (other != _tab && _scanned[other] && _autoAnalyze[other] && _scanningTab != other) {
                keys = keysToAnalyzeLocked(other);
                tab = other;
            }
        }
    }
    if (keys.empty())
        return;
    _analyst.join();
    if (!_workerRunning)
        _sink->stop = false;
    _analyzingTab = tab;
    _analystRunning = true;
    _dirty = true;
    _analyst.start(
        [this, keys = std::move(keys)] {
            remote::setGiveWay([this] { return _sink->stop.load(); });
            try {
                analyze(keys);
            } catch (const Cancelled&) {
                logLine("Stopped analyzing");
            } catch (const std::exception& e) {
                logLine(std::string("意外失败：") + e.what());
            } catch (...) {
                logLine("意外失败");
            }
            _analystRunning = false;
            _dirty = true;
        },
        -1, 1 << 21);
}

/// What goes on by itself whenever nothing else is: finding out what's in each list as it comes up, and what each game would gain.
void App::startWhatsDue() {
    if ((_screen != Screen::Main && _screen != Screen::Welcome) || _dialog || _menu)
        return;
    // Asked to stop, it first has to have stopped.
    if (_workerRunning) {
        if (_jobKind == JobKind::Scanning && !_analystRunning && !_sink->stop && !_afterWork)
            startAnalyzing();
        return;
    }
    // The list on the screen first, and then any other that didn't get to finish.
    int toScan = !_scanned[_tab] && _tab != TabSettings ? _tab : -1;
    for (int other = 0; other < TabSettings && toScan < 0; other++) {
        if (_scanWanted[other] && !_scanned[other])
            toScan = other;
    }
    // What's been asked for goes before any list, once there's something to do it to. The lists pick up again afterwards.
    if (_afterWork && (toScan < 0 || canActNow())) {
        if (_analystRunning)
            return;
        auto action = std::move(_afterWork);
        _afterWork = nullptr;
        confirmTicked();
        action();
        return;
    }
    if (toScan >= 0) {
        int tab = toScan;
        _scanWanted[tab] = true;
        _scanningTab = tab;
        runInBackground(JobKind::Scanning, [this, tab] {
            try {
                if (tab == TabInstalled)
                    scanInstalled();
                else
                    scanFiles();
            } catch (const std::exception& e) {
                // Trying again straight away wouldn't change anything, so it counts as done.
                logLine(std::string("无法遍历列表：") + e.what());
            }
            _scanned[tab] = true;
            _scanWanted[tab] = false;
        });
        return;
    }
    if (!_analystRunning)
        startAnalyzing();
}

// -------------------------------------------------------------------------------------------------------------------
// Updating

void App::startUpdateCheck(bool byHand) {
    if (_updating.exchange(true))
        return;
    _updateThread.join();
    {
        sys::Lock lock(_mutex);
        _updateState = "正在查找";
        _dirty = true;
    }
    _updateThread.start(
        [this, byHand] {
            try {
                checkForUpdate(byHand);
            } catch (const ToolError& e) {
                logLine(std::string("正在更新：") + e.what());
                sys::Lock lock(_mutex);
                _updateState = "无法完成";
                if (byHand) {
                    _updateNoticeTitle = "操作未成功";
                    _updateNoticeText = e.what();
                }
            } catch (const std::exception& e) {
                logLine(std::string("正在更新：") + e.what());
                sys::Lock lock(_mutex);
                _updateState = "无法完成";
            }
            _updating = false;
            _dirty = true;
        },
        -1, 1 << 20);
}

/// Everything from the release is checked against a signature that only whoever holds the private key can have made, so it doesn't matter
/// what happens to it on its way here. The program is only replaced once all of it has been checked, and the old one stays until the new
/// one is in place.
void App::checkForUpdate(bool byHand) {
    auto keepGoing = [this] { return !_updateStop.load(); };
    std::string path = _platform.programPath();
    if (path.empty())
        throw ToolError("当前副本无法确认自身存放位置，因此无法自我更新");
    if (!_platform.isOnline()) {
        sys::Lock lock(_mutex);
        _updateState = "未联网";
        if (byHand) {
            _updateNoticeTitle = "Not connected";
            _updateNoticeText = "此主机未连接互联网。";
        }
        return;
    }

    Bytes listing = _platform.download(ReleasesUrl, 1 << 20, keepGoing);
    std::string text(listing.begin(), listing.end());
    constexpr const char* TagField = "\"tag_name\":\"";
    size_t at = text.find(TagField);
    if (at == std::string::npos)
        throw ToolError("获取的版本列表格式与预期不符");
    at += strlen(TagField);
    std::string tag = text.substr(at, text.find('"', at) - at);
    std::string version = !tag.empty() && tag[0] == 'v' ? tag.substr(1) : tag;
    if (!isNewerVersion(version, AppVersion)) {
        logLine(format("Version %s is the latest there is, and this is %s", version.c_str(), AppVersion));
        sys::Lock lock(_mutex);
        _updateState = "已是最新版本";
        return;
    }
    logLine("版本" + version + "已发布");
    {
        sys::Lock lock(_mutex);
        _updateState = "正在获取版本" + version;
        _dirty = true;
    }

    std::string base = DownloadsUrl + tag + "/downloads/";
    Bytes manifest = _platform.download(base + UpdateManifestName, 1 << 16, keepGoing);
    Bytes signature = _platform.download(base + UpdateSignatureName, 1 << 12, keepGoing);
    if (signature.size() != 0x100 || !verifyUpdateSignature(manifest.data(), manifest.size(), signature.data()))
        throw ToolError("该版本的签名校验未通过，因此未安装");

    std::string name = _platform.releaseFileName();
    std::string expected, signedVersion;
    forEachLine(std::string(manifest.begin(), manifest.end()), [&](const std::string& line) {
        if (line.rfind("version ", 0) == 0)
            signedVersion = line.substr(8);
        else if (line.size() > 66 && line.compare(64, 2, "  ") == 0 && line.substr(66) == name)
            expected = line.substr(0, 64);
    });
    if (signedVersion != version)
        throw ToolError("该版本自报的版本号与列表中的不一致，因此未安装");
    if (expected.empty())
        throw ToolError("该版本没有适用于当前程序版本的内容");

    Bytes program = _platform.download(base + name, LargestProgram, keepGoing);
    u8 hash[32];
    sha256(program.data(), program.size(), hash);
    if (toHex(hash, 32) != expected)
        throw ToolError("下载内容与该版本声明的不一致，因此未安装");

    std::string fresh = path + ".new", old = path + ".old";
    {
        sys::File file(fresh, sys::File::Mode::Create);
        file.write(0, program.data(), program.size());
        file.flush();
    }
    sys::commit(fresh);
    Bytes back = readBytesFile(fresh);
    if (back != program) {
        sys::removeFile(fresh);
        throw ToolError("新版本回读校验不一致，因此未安装；存储卡可能有问题");
    }
    sys::removeFile(old);
    sys::renameFile(path, old);
    try {
        sys::renameFile(fresh, path);
    } catch (const ToolError&) {
        sys::renameFile(old, path);
        throw;
    }
    sys::removeFile(old);
    sys::commit(path);
    logLine("已更新到版本" + version);

    sys::Lock lock(_mutex);
    _updateState = "已更新到" + version;
    _updateNoticeTitle = "已更新到版本" + version;
    _updateNoticeText = "以后启动时将使用该版本。";
    _updatedProgram = path;
}

}  // namespace hbp
