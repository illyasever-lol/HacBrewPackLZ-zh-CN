// Starting up and closing down, the log, the threads the work runs on, and updating the app itself.
#include "app_internal.h"

#include <cstdio>

#include "../core/remote.h"

namespace hbp {

using namespace app;

namespace {

constexpr const char* ReleasesUrl = "https://gitlab.com/api/v4/projects/ismaellozanoperezserrano%2Fhacbrewpacklz/releases?per_page=1";
/// Test versions are released in the same project, dated just before its newest release, so the newest release by date, which is all everyone
/// else's copy looks at, and all every copy before 1.5 looks at, stays the newest release. A test version's copy looks through them all.
constexpr const char* AllReleasesUrl = "https://gitlab.com/api/v4/projects/ismaellozanoperezserrano%2Fhacbrewpacklz/releases?per_page=100";
constexpr const char* DownloadsUrl = "https://gitlab.com/ismaellozanoperezserrano/hacbrewpacklz/-/releases/";
constexpr const char* TestMark = "-test.";
constexpr const char* UpdateManifestName = "hacbrewpacklz-update.txt";
constexpr const char* UpdateSignatureName = "hacbrewpacklz-update.sig";
constexpr size_t LargestProgram = 64 << 20;

/// Whether the first version comes after the second, going by up to three numbers. A test version, such as 1.5.0-test.2, comes after the test
/// versions before it and before the release with its number, so testers move on to that once it's out.
bool isNewerVersion(const std::string& a, const std::string& b) {
    int x[4] = {0, 0, 0, INT32_MAX}, y[4] = {0, 0, 0, INT32_MAX};
    sscanf(a.c_str(), "%d.%d.%d", &x[0], &x[1], &x[2]);
    sscanf(b.c_str(), "%d.%d.%d", &y[0], &y[1], &y[2]);
    size_t testA = a.find(TestMark), testB = b.find(TestMark);
    if (testA != std::string::npos)
        x[3] = atoi(a.c_str() + testA + strlen(TestMark));
    if (testB != std::string::npos)
        y[3] = atoi(b.c_str() + testB + strlen(TestMark));
    for (int i = 0; i < 4; i++) {
        if (x[i] != y[i])
            return x[i] > y[i];
    }
    return false;
}

bool isTestVersion(const std::string& version) { return version.find(TestMark) != std::string::npos; }

/// Whether this is the thread that analyzes and measures, beside the one for jobs and looking through lists, which both tell the same sink how
/// they're getting on.
thread_local bool t_analyst = false;

/// How often log-other.txt is given its usual name again, while the system turns log.txt down (nameLogLocked).
constexpr double LogNameEvery = 5;

}  // namespace

/// How the job on the worker thread tells the screen how it's getting on.
class App::Sink : public JobSink {
public:
    explicit Sink(App& app) : _app(app) {}

    void message(const std::string& text) override { _app.logLine(text); }
    void warning(const std::string& text) override { _app.logLine("警告： " + text); }

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
        // The analyst's is how far it's got through the game it's measuring, image by image, which a list being looked through at the same
        // time mustn't take for its own.
        if (t_analyst) {
            {
                sys::Lock lock(_app._mutex);
                if (total != _app._analysisImageTotal || done < _app._analysisImageDone) {
                    _app._analysisImagesDone += _app._analysisImageDone;
                    _app._analysisImageTotal = total;
                }
                _app._analysisImageDone = done;
            }
            // Between one part of a game being read in full and the next, a game that needs a first look gets it, the reading waiting.
            _app.firstLooksWhileMeasuring();
            return;
        }
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
    _reportThread.join();
    closingProblems();
    {
        sys::Lock lock(_mutex);
        if (_cacheChanged)
            saveCacheLocked();
    }
    saveFileIndex();
    // The views the lists were left on, saved the once, as the app closes, rather than each time one changes.
    if (_viewsChanged) {
        _viewsChanged = false;
        saveSettings();
    }
    if (_working) {
        _working = false;
        _platform.setWorking(false, false);
    }
    if (!_closedDown && _log) {
        _closedDown = true;
        logLine("正在关闭");
        logFolder(_platform.dataFolder());
    }
}

// -------------------------------------------------------------------------------------------------------------------
// The log

void App::logLine(const std::string& text) {
    sys::Lock lock(_logMutex);
    std::string line = format("[%7.1f] %s\n", sys::now(), text.c_str());
    constexpr size_t RecentLines = 3000;
    _recentLog.push_back(line);
    if (_recentLog.size() > RecentLines)
        _recentLog.pop_front();
    if (!_logDecided) {
        _earlyLog.push_back(line);
        return;
    }
    if (_logPath.empty())
        return;
    // Said as soon as it's known rather than at the end, which a console that goes down never gets to.
    sys::Refusals refused = sys::refusals();
    if (refused.total() > _refusalsLogged) {
        _refusalsLogged = refused.total();
        // Which file or folder says more about why than any count.
        line += format("[%7.1f] The system has turned down %lld requests to the card so far, %lld as out of handles (2001-0105) and %lld for an invalid memory "
                       "state (2001-0106), each asked once. The last was for %s\n", sys::now(), (long long)refused.total(), (long long)refused.outOfHandles,
                       (long long)refused.memoryState, refused.lastFor.c_str());
    }
    // Each one in detail, the first few times, for working out why it happens.
    for (auto& detail : sys::takeRefusalDetails())
        line += format("[%7.1f] %s\n", sys::now(), detail.c_str());
    // Kept until the card lets it be written, a few hundred KB at most, the newest.
    constexpr size_t MostUnwritten = 256 << 10;
    _logUnwritten += line;
    if (_logUnwritten.size() > MostUnwritten)
        _logUnwritten.erase(0, _logUnwritten.size() - MostUnwritten);
    double now = sys::now();
    // Each request to the card that's turned down is a chance of custom firmware's fs.mitm taking the console down (askCard in nx_sys.cpp), so
    // a log turned down isn't asked for again for a few seconds.
    constexpr double RetryAfter = 5;
    try {
        // The card turned down opening it again, which some cards do now and then: it's asked again a few seconds later, and nothing said in
        // between is lost. It used to stop the log for good, so all that was in it was the app opening.
        if (!_log) {
            if (now < _logRetryAt)
                return;
            _logClosedAt = now;
            _log.reset(new sys::File(_logPath, _logPosition > 0 ? sys::File::Mode::Update : sys::File::Mode::Create));
        }
        _log->write(_logPosition, _logUnwritten.data(), _logUnwritten.size());
        _logPosition += (i64)_logUnwritten.size();
        _logUnwritten.clear();
        _log->flush();
        // If the console goes down, what's in a file that's still open can go with it, and the file too. Going through the lists is where that's been seen.
        // Closing it costs the card a change to its folder each time, so it's at most once a second, even while the lines come thick and fast.
        if (now - _logClosedAt >= 1) {
            _logClosedAt = now;
            _log.reset();
            sys::commit(_logPath);
            nameLogLocked();
            _log.reset(new sys::File(_logPath, sys::File::Mode::Update));
        }
    } catch (const ToolError&) {
        // A log that can't be written isn't worth stopping for, nor giving up on: it's tried again a few seconds later.
        _log.reset();
        _logRetryAt = now + RetryAfter;
    }
}

/// There's only ever the one log, of this time. Earlier versions kept more, which go.
void App::removeOldLogs() {
    std::string folder = _platform.dataFolder();
    for (const char* old : {"/log.1.txt", "/log.2.txt", "/log-other.txt", "/failure.txt"})
        sys::removeFile(folder + old);
}

void App::openLog() {
    // Whatever an earlier log was waiting to do, as Keep a log can be turned off and on again: renaming log-other.txt then would delete the
    // log.txt this one is.
    _logNameTries = 0;
    _logNameNextTry = _logRetryAt = 0;
    try {
        removeOldLogs();
        _logPath = _platform.dataFolder() + "/log.txt";
        try {
            _log.reset(new sys::File(_logPath, sys::File::Mode::Create));
        } catch (const ToolError& e) {
            // A console has been seen to turn this one name down each time the app opens, which is no reason to go without: it's kept as
            // log-other.txt, which gets the usual name once the system lets it (nameLogLocked), every few seconds for about a minute, as
            // asking straight away would only be turned down again.
            _logProblem = e.what();
            _logPath = _platform.dataFolder() + "/log-other.txt";
            _log.reset(new sys::File(_logPath, sys::File::Mode::Create));
            constexpr int NameTries = 12;
            _logNameTries = NameTries;
            _logNameTurnedDownAt = sys::now();
            _logNameNextTry = _logNameTurnedDownAt + LogNameEvery;
            logLine(_logProblem);
            _logProblem.clear();
        }
    } catch (const ToolError& e) {
        _logProblem = e.what();
        _log.reset();
        _logPath.clear();
    }
}

/// The log kept as log-other.txt, as the system turned log.txt down when the app opened, gets the name it's looked for by once the system lets
/// it, which it has been seen to a few seconds on. Renamed while it's closed, which writes nothing. Only every few seconds, as each time it's
/// turned down is one more refused request to the card, and only so many times, as there's no telling a name turned down for good.
void App::nameLogLocked() {
    std::string usual = _platform.dataFolder() + "/log.txt";
    // Only ever log-other.txt: the log already being log.txt, renaming would delete it.
    if (_logNameTries <= 0 || _logPath == usual || sys::now() < _logNameNextTry)
        return;
    _logNameTries--;
    _logNameNextTry = sys::now() + LogNameEvery;
    try {
        // There's one from an earlier time if deleting it was turned down too.
        sys::removeFile(usual);
        sys::renameFile(_logPath, usual);
    } catch (const ToolError&) {
        return;
    }
    _logPath = usual;
    _logNameTries = 0;
    std::string line = format("[%7.1f] Renamed log-other.txt to log.txt, as the system let that name be made %.1f s after turning it down\n", sys::now(),
                              sys::now() - _logNameTurnedDownAt);
    _recentLog.push_back(line);
    _logUnwritten += line;
}

/// What's there, as it's the only way of telling afterwards what became of something that was saved.
void App::logFolder(const std::string& folder) {
    std::string files;
    for (auto& entry : sys::list(folder)) {
        if (!entry.isDirectory)
            files += format("%s%s %lld", files.empty() ? "" : ", ", entry.name.c_str(), (long long)entry.size);
    }
    logLine("位于 " + folder + ": " + (files.empty() ? std::string("nothing") : files));
}

void App::noteUnexpected(const std::string& what) {
    logLine("意外失败： " + what);
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
            logLine("密钥来自 " + path);
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
    // Something goes on the screen first, which takes only the fonts and whether it's dark.
    _platform.loadFonts(_font);
    _mono.addFace(InconsolataRegular, InconsolataRegularSize);
    _platform.loadFonts(_mono);
    _dark = _platform.prefersDark();
    if (whenItCanBeDrawn)
        whenItCanBeDrawn();

    loadSettings();
    // Each list opens on the view it was last on: To do the first time, or Everything for someone who had turned off "Hide games that are
    // done", which this was before.
    for (int tab = 0; tab < TabCount; tab++)
        _view[tab] = _settings.view[tab] >= 0 ? (View)_settings.view[tab] : _settings.hideCompressed ? View::ToDo : View::Everything;
    if (_settings.writeLog)
        openLog();
    {
        sys::Lock lock(_logMutex);
        _logDecided = true;
    }
    for (auto& line : std::vector<std::string>(std::move(_earlyLog)))
        logLine(line.substr(line.find(']') + 2, line.size() - line.find(']') - 3));
    logLine(format("hacBrewPackLZ for Switch %s, built %s", AppVersion, builtAt()));
    logLine(_platform.aboutSystem());
    logFolder(_platform.dataFolder());
    logFolder(_platform.backupFolder());

    _memoryLimited = _platform.memoryIsLimited();
    watchMemory();
    // A solid NSZ read out of order is kept in memory as far as there's room for it, and only the rest goes in a temporary file on the card.
    NczSource::setSpoolMemory(solidMemory());
    loadCache();
    loadHidden();
    buildOptions();
    buildWelcome();
    loadKeys();
    checkLastTime();
    _drives = _platform.drives();

    _console = _platform.console();
    if (!_console) {
        logLine("主机服务不可用： " + _platform.consoleProblem());
        _tab = TabFiles;
        _scanned[TabInstalled] = true;
    }
    if (_keysPath.empty()) {
        _screen = Screen::NoKeys;
        return;
    }
    // With the app's own folder gone, it's being set up afresh, even if the settings can be had from their other place. Only settings known to
    // be missing count: the card turning the question down for a few seconds brought the welcome back for someone who'd long had it.
    std::string settingsHere = _platform.dataFolder() + "/settings.txt";
    if (sys::presence(settingsHere) == sys::Presence::Missing && sys::presence(settingsHere + ".new") == sys::Presence::Missing)
        _settings.welcomed = false;
    if (!_settings.welcomed)
        _screen = Screen::Welcome;
    else if (!_settings.problemsAsked)
        askAboutSendingProblems();
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
    // What the system turned down as the app opened is read again, as by now it often isn't, and before anything goes over the lists. It can go
    // on being turned down for a few seconds, and it's worth waiting for: without it, every game's name and icon, and every game file, is read
    // again.
    for (int attempt = 0; attempt < 6 && (_listRefused || _indexRefused); attempt++) {
        if (attempt > 0)
            sys::sleepMs(1000);
        if (_listRefused.exchange(false)) {
            loadList();
            if (!_listRefused) {
                size_t remembered;
                {
                    sys::Lock lock(_mutex);
                    remembered = _rows.size();
                }
                logLine(format("Read again what was remembered: %d games on the lists", (int)remembered));
            }
        }
        if (_indexRefused.exchange(false))
            loadFileIndex();
    }
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
        _journalChecked = true;
    } catch (const ToolError& e) {
        logLine(std::string("无法清理上次的遗留内容： ") + e.what());
        sys::Lock lock(_mutex);
        _notes.push_back(std::string("What the last job may have left unfinished couldn't be dealt with yet. It will be tried again before anything is replaced. ") + e.what());
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
                logLine("已停止");
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
    bool measuring = false;
    {
        sys::Lock lock(_mutex);
        // The list on the screen first, and then the others that have come up, so that it's not left waiting for anyone to go and look at them.
        if (_autoAnalyze[_tab])
            keys = keysToAnalyzeLocked(_tab);
        if (!keys.empty())
            tab = _tab;
        for (int other = 0; other < TabCount && keys.empty(); other++) {
            if (other != _tab && _scanned[other] && _autoAnalyze[other] && _scanningTab != other) {
                keys = keysToAnalyzeLocked(other);
                tab = other;
            }
        }
        // Thorough, once every game has had its first look, every game is read in full, writing nothing, the list on the screen first, and on
        // it the games it could change the answer for first. Not on a low battery, which is only asked about now and then.
        if (keys.empty() && _settings.thorough && _settings.compress && sys::now() >= _measureAgainAt && !firstLooksLeftLocked()) {
            for (int i = 0; i < TabCount && keys.empty(); i++) {
                int each = (_tab + i) % TabCount;
                if (_scanned[each] && _autoAnalyze[each] && _scanningTab != each) {
                    keys = measureKeysLocked(each);
                    tab = each;
                }
            }
            measuring = !keys.empty();
        }
    }
    if (keys.empty())
        return;
    if (measuring && !batteryTooLow().empty()) {
        _measureAgainAt = sys::now() + 60;
        return;
    }
    _analyst.join();
    if (!_workerRunning)
        _sink->stop = false;
    _analyzingTab = tab;
    _analystMeasuring = measuring;
    _analystStartedMeasuring = measuring;
    _analystRunning = true;
    _dirty = true;
    _analyst.start(
        [this, keys = std::move(keys), measuring] {
            t_analyst = true;
            remote::setGiveWay([this] { return _sink->stop.load(); });
            try {
                analyze(keys, measuring);
            } catch (const Cancelled&) {
                logLine(measuring ? "Stopped measuring" : "已停止分析");
            } catch (const std::exception& e) {
                logLine(std::string("意外失败： ") + e.what());
            } catch (...) {
                logLine("意外失败");
            }
            _analystMeasuring = false;
            _analystStartedMeasuring = false;
            _analystRunning = false;
            _dirty = true;
        },
        -1, 1 << 21);
}

/// What goes on by itself whenever nothing else is: finding out what's in each list as it comes up, and what each game would gain.
void App::startWhatsDue() {
    if ((_screen != Screen::Main && _screen != Screen::Welcome) || _dialog || _menu)
        return;
    // What X asked for has been answered, backed out of, or done, so analysis that gave way to it goes on.
    if (_analysisPausedTab >= 0 && !_afterWork && !(_workerRunning && _jobKind != JobKind::Scanning)) {
        _autoAnalyze[_analysisPausedTab] = true;
        _analysisPausedTab = -1;
    }
    // Asked to stop, it first has to have stopped.
    if (_workerRunning) {
        if (_jobKind == JobKind::Scanning && !_analystRunning && !_sink->stop && !_afterWork)
            startAnalyzing();
        return;
    }
    // The list on the screen first, and then any other that didn't get to finish. The overview counts what's on both.
    int toScan = !_scanned[_tab] ? _tab : -1;
    for (int other = 0; other < TabCount && toScan < 0; other++) {
        if ((_scanWanted[other] || _overview) && !_scanned[other])
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
            _scanStoppedByHand = false;
            try {
                if (tab == TabInstalled)
                    scanInstalled();
                else
                    scanFiles();
            } catch (const Cancelled&) {
                // Having given way to another list, it picks up again afterwards. Stopped with B, what's been found is the list until it's refreshed.
                if (!_scanStoppedByHand.exchange(false))
                    throw;
                std::vector<std::string> roots = tab == TabFiles ? fileRoots() : std::vector<std::string>();
                {
                    sys::Lock lock(_mutex);
                    if (tab == TabFiles)
                        showIndexRowsLocked(indexedPathsLocked(roots), true);
                    sortRowsLocked();
                    _dirty = true;
                }
                logLine(tab == TabInstalled ? "Stopped looking through the installed games" : "Stopped looking for game files");
                _sayStoppedLooking = true;
            } catch (const std::exception& e) {
                // Trying again straight away wouldn't change anything, so it counts as done.
                logLine(std::string("无法遍历列表： ") + e.what());
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
        _updateState = "Looking";
        _dirty = true;
    }
    _updateThread.start(
        [this, byHand] {
            try {
                checkForUpdate(byHand);
            } catch (const ToolError& e) {
                logLine(std::string("正在更新： ") + e.what());
                sys::Lock lock(_mutex);
                _updateState = "无法完成";
                if (byHand) {
                    _updateNoticeTitle = "操作未成功";
                    _updateNoticeText = e.what();
                }
            } catch (const std::exception& e) {
                logLine(std::string("正在更新： ") + e.what());
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
            _updateNoticeTitle = "未连接";
            _updateNoticeText = "此主机未连接互联网。";
        }
        return;
    }

    // Everyone's copy goes by the newest release by date, which a test version is never dated after. A test version's copy looks through every
    // release for the newest version, test versions too, and so moves on to the release once it's out.
    auto versionOf = [](const std::string& tag) { return !tag.empty() && tag[0] == 'v' ? tag.substr(1) : tag; };
    bool testing = isTestVersion(AppVersion);
    Bytes listing = _platform.download(testing ? AllReleasesUrl : ReleasesUrl, testing ? 4 << 20 : 1 << 20, keepGoing);
    std::string text(listing.begin(), listing.end());
    constexpr const char* TagField = "\"tag_name\":\"";
    std::string tag;
    for (size_t at = text.find(TagField); at != std::string::npos; at = text.find(TagField, at)) {
        at += strlen(TagField);
        size_t end = text.find('"', at);
        if (end == std::string::npos)
            break;
        std::string found = text.substr(at, end - at);
        if (tag.empty() || isNewerVersion(versionOf(found), versionOf(tag)))
            tag = found;
        at = end;
    }
    if (tag.empty())
        throw ToolError("获取的版本列表格式与预期不符");
    const char* downloads = DownloadsUrl;
    std::string version = versionOf(tag);
    if (!isNewerVersion(version, AppVersion)) {
        logLine(format("Version %s is the latest there is, and this is %s", version.c_str(), AppVersion));
        sys::Lock lock(_mutex);
        _updateState = "已是最新版本";
        return;
    }
    logLine("版本 " + version + " 已发布");
    {
        sys::Lock lock(_mutex);
        _updateState = "正在获取版本 " + version;
        _dirty = true;
    }

    std::string base = downloads + tag + "/downloads/";
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
    // A request the card turns down goes through a moment later, so each rename is asked again a few times before it's given up: turned down
    // twice in a row, the program was left only as .old and .new, which the homebrew menu doesn't start.
    auto renameAgain = [](const std::string& from, const std::string& to) {
        for (int tries = 1;; tries++) {
            try {
                sys::renameFile(from, to);
                return;
            } catch (const ToolError& e) {
                if (tries >= 4 || !refusedBySystem(e.what()))
                    throw;
                sys::sleepMs(1000 * tries);
            }
        }
    };
    // Left only as .old by a rename turned down last time, the program goes back under its own name first, rather than being deleted as an old one.
    if (sys::presence(path) == sys::Presence::Missing && sys::presence(old) == sys::Presence::There)
        renameAgain(old, path);
    sys::removeFile(old);
    renameAgain(path, old);
    try {
        renameAgain(fresh, path);
    } catch (const ToolError&) {
        try {
            renameAgain(old, path);
        } catch (const ToolError& e) {
            throw ToolError("The new version couldn't be put in place, and the card turned putting this one back down too (" + std::string(e.what()) +
                            "). Update again in a moment, before closing the app: until then the homebrew menu won't find it. On a PC, '" + fileNameOf(old) +
                            "' can be renamed back to '" + fileNameOf(path) + "'");
        }
        throw;
    }
    sys::removeFile(old);
    sys::commit(path);
    logLine("已更新到版本 " + version);

    sys::Lock lock(_mutex);
    _updateState = "已更新到 " + version;
    _updateNoticeTitle = "已更新到版本 " + version;
    _updateNoticeText = "以后启动时将使用该版本。";
    _updatedProgram = path;
}

}  // namespace hbp
