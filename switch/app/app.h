// The Switch version: lists of what's on the console, and of game files wherever they're looked for, how much each would gain, and doing
// something about it. It knows nothing of the console itself: everything it needs comes through Platform and ConsoleApi, so all of it runs
// on a PC too.
#pragma once

#include <map>
#include <set>

#include "../core/console.h"
#include "canvas.h"

namespace hbp {

constexpr const char* AppVersion = "1.4.3";

enum Button : u32 {
    ButtonA = 1u << 0,
    ButtonB = 1u << 1,
    ButtonX = 1u << 2,
    ButtonY = 1u << 3,
    ButtonL = 1u << 4,
    ButtonR = 1u << 5,
    ButtonZL = 1u << 6,
    ButtonZR = 1u << 7,
    ButtonPlus = 1u << 8,
    ButtonMinus = 1u << 9,
    ButtonUp = 1u << 10,
    ButtonDown = 1u << 11,
    ButtonLeft = 1u << 12,
    ButtonRight = 1u << 13,
    ButtonStickL = 1u << 14,
    ButtonStickR = 1u << 15,
};

struct InputState {
    u32 held = 0;
    bool touching = false;
    int touchX = 0, touchY = 0;
};

/// Somewhere files can be, with a name people know it by.
struct Drive {
    std::string name;
    std::string root;
    bool isUsb = false;
};

class Platform {
public:
    virtual ~Platform() = default;
    /// Null if the console's services can't be had, with the reason from consoleProblem.
    virtual ConsoleApi* console() = 0;
    virtual std::string consoleProblem() = 0;
    virtual void loadFonts(Font& font) = 0;
    virtual bool decodeIcon(const Bytes& jpeg, Image& out) = 0;
    virtual bool prefersDark() = 0;
    /// Where settings, the log and temporary data go.
    virtual std::string dataFolder() = 0;
    /// Somewhere else again for a copy of what's not worth losing.
    virtual std::string backupFolder() = 0;
    /// Where NSPs go, and extracted games in a folder within, unless told otherwise.
    virtual std::string outputFolder() = 0;
    /// What's there to look through and choose from, which can change at any time.
    virtual std::vector<Drive> drives() = 0;
    /// Something has been plugged in or taken out since the last time this was asked.
    virtual bool drivesChanged() = 0;
    /// Folders that are the system's own, and not looked through for game files.
    virtual std::vector<std::string> foldersToSkip() = 0;
    virtual std::vector<std::string> keyLocations() = 0;
    /// Nothing should sleep or dim while something's being done.
    virtual void setWorking(bool working, bool boost) = 0;
    /// Negative if there's no telling.
    virtual int batteryPercent() = 0;
    virtual bool isCharging() = 0;
    /// Opened in a way that leaves little memory to work with, which a large solid NSZ may not fit in.
    virtual bool memoryIsLimited() = 0;
    /// The firmware has to be new enough to read the compression this stores games with.
    virtual bool firmwareReadsCompression() = 0;
    /// A line for the log about what this is running on.
    virtual std::string aboutSystem() = 0;

    /// Whether the console is connected to the internet at all.
    virtual bool isOnline() = 0;
    /// What's at an address, as long as it isn't more than the limit and keepGoing stays true. Throws if it can't be had.
    virtual Bytes download(const std::string& url, size_t limit, const std::function<bool()>& keepGoing) = 0;
    /// The file this program is, or empty if there's no telling, which means it can't update itself.
    virtual std::string programPath() = 0;
    /// What this program is called among the files of a release.
    virtual std::string releaseFileName() = 0;
    /// Starts that file again once this one has ended. False if it can't be done from here.
    virtual bool restartAfterExit(const std::string& path) = 0;
};

class App {
public:
    explicit App(Platform& platform);
    ~App();

    void start();
    void update(const InputState& input, double dt);
    void draw(Canvas& canvas);
    /// Whether something on screen has changed since the last draw.
    bool needsDrawing();
    bool wantsToQuit() const { return _quit; }
    bool showingWelcome() const { return _screen == Screen::Welcome; }
    /// The app is about to be closed: stops what's running, at the next safe point, and waits for it.
    void shutdown();
    bool busy() const { return _workerRunning || _analystRunning; }

    /// What's going on, in words, for the tests that run on a PC.
    struct RowView {
        std::string name;
        bool ticked = false;
        bool hasEstimate = false;
        bool compressed = false;
        std::string note;
        std::string detail;
        int notes = 0;
    };
    struct Snapshot {
        std::string screen;
        int tab = 0;
        std::string label;
        bool analyzing = false;
        bool scanning = false;
        std::string dialog;
        std::string dialogText;
        std::string toast;
        std::string sortedBy;
        std::string tickHint;
        std::string actionWord;
        std::string resultsTitle;
        std::string resultsDetail;
        std::vector<std::string> results;
        std::vector<RowView> rows;
        int compressedHidden = 0;
        double overall = 0;
        std::string browsing;
        std::vector<std::string> browseEntries;
        std::vector<std::string> options;
        int optionFocus = 0;
        std::vector<std::string> menu;
        int menuFocus = 0;
        std::vector<std::string> welcome;
        int welcomeFocus = 0;
        std::string updateState;
    };
    Snapshot snapshot();

private:
    enum class Screen { Main, Job, Results, NoKeys, Browse, Welcome };
    enum class RowState { Ready, Working, Done, Failed };
    enum class JobKind { None, Starting, Scanning, Compressing, Extracting, Checking, Removing };
    enum class TickAction { All, None, WorthIt };
    enum { MenuMain, MenuThisGame, MenuTicked, MenuList, MenuSources };

    static constexpr int TabInstalled = 0;
    /// Game files and the folders of extracted games, wherever they're looked for: they're all games that aren't installed.
    static constexpr int TabFiles = 1;
    static constexpr int TabSettings = 2;
    static constexpr int TabCount = 3;

    struct Row {
        std::string key;
        int tab = 0;
        u64 application = 0;
        std::string name;
        /// The version people know it by.
        std::string version;
        /// The game's own version, with its update if there is one, to tell whether a file is newer than what's installed.
        u32 gameVersion = 0;
        std::string detail;
        std::string fingerprint;
        std::shared_ptr<Image> icon;
        std::shared_ptr<Image> bigIcon;
        Color tile;
        i64 now = 0;
        /// A game that small with nothing else to it, like a forwarder.
        bool tiny = false;
        /// There's an update to make part of it, which is what repacking gains anything from.
        bool hasUpdate = false;
        /// It's known to be there as described, rather than only remembered from last time.
        bool ready = false;
        bool ticked = true;
        bool byHand = false;
        bool hasEstimate = false;
        SizeEstimate estimate;
        std::string belowMinimum;
        RowState state = RowState::Ready;
        std::string message;
        i64 freed = 0;
        std::string blocked;
        Storage storage = Storage::None;
        /// The files a game file's row comes from, and which of the titles in them it is.
        std::string mainFile;
        std::string updateFile;
        u64 titleId = 0;
        /// The game's DLC, each from its file, which go with it wherever it goes.
        std::vector<std::string> dlcFiles;
        std::vector<u64> dlcIds;
        /// What there is to know about it that isn't the game itself, such as other copies of it that were left aside.
        std::vector<std::string> notes;
        /// The folder of an extracted game, which is built rather than converted.
        std::string extracted;
    };

    struct Settings {
        int minSaving = 5;
        bool hideSmall = true;
        bool hideCompressed = true;
        /// Where the list of game files looks: the SD card, USB drives, and PCs running hacbrewpacklz send on the network or the cable.
        bool fromSd = true;
        bool fromUsb = true;
        bool fromPcNetwork = true;
        bool fromPcCable = true;
        bool compress = true;
        int level = 9;
        int blockSize = 0x10000;
        bool replaceInstalled = true;
        bool installFiles = true;
        /// Where a game that isn't installed yet goes. One that is stays where it is.
        bool installToConsole = false;
        bool deleteInstalled = false;
        bool boost = true;
        bool warned = false;
        int sort = 0;
        bool autoUpdate = true;
        bool checkResult = false;
        /// The welcome screen has been and gone.
        bool welcomed = false;
        /// The settings that rarely need changing are showing.
        bool moreSettings = false;
        /// Only when it's wanted, as it takes writing to the card.
        bool writeLog = false;
        /// Empty means the usual place.
        std::string nspFolder;
        std::string extractFolder;
        std::string filesFolder;
    };
    /// The settings that are on or off, and the folders, with the names they're saved under.
    static const std::pair<const char*, bool Settings::*> SavedFlags[];
    static const std::pair<const char*, std::string Settings::*> SavedFolders[];

    struct OptionItem {
        std::string section;
        std::string label;
        std::string help;
        std::function<std::string()> value;
        std::function<void(int)> change;
        /// It's something to do rather than something to set.
        bool action = false;
        /// Holding left or right keeps going, which is only any use for what has a lot of values.
        bool repeats = false;
    };

    struct Dialog {
        std::string title;
        std::vector<std::string> paragraphs;
        struct Choice {
            std::string label;
            std::function<void()> action;
        };
        std::vector<Choice> choices;
        int focus = 0;
        int cancelChoice = -1;
    };

    struct Menu {
        struct Item {
            std::string label;
            std::function<void()> action;
            /// For something to set with left and right, rather than something to do.
            std::function<std::string()> value;
            std::function<void(int)> change;
        };
        std::vector<Item> items;
        int focus = 0;
        /// B goes back from a page to the first one.
        int page = 0;
    };
    struct MenuFacts;

    struct ResultLine {
        std::string name;
        std::string outcome;
        RowState state = RowState::Done;
        std::string message;
        i64 before = 0, after = 0;
        std::shared_ptr<Image> icon;
        Color tile;
    };

    /// Somewhere on the screen that does something when it's touched.
    struct Hit {
        Rect area;
        std::function<void()> action;
    };

    /// A button and what it does, along the bottom of the screen.
    struct Hint {
        const char* button;
        std::string label;
        std::function<void()> action;
        bool enabled = true;
    };

    /// Choosing a folder.
    struct Browser {
        struct Entry {
            std::string name;
            std::string path;
            std::string note;
        };
        std::string title;
        std::string path;
        std::vector<Entry> entries;
        std::string problem;
        int focus = 0;
        int lastFocus = 0;
        double scroll = 0;
        double scrollShown = 0;
        std::string useDefaultLabel;
        std::function<void(const std::string&)> choose;
    };

    /// What's known about a title in a game file, or an extracted game, without having to open it to find that out again.
    struct IndexedTitle {
        u64 id = 0;
        u8 type = 0;
        u32 version = 0;
        u64 linked = 0;
        i64 bytes = 0;
        bool holdsOnlyPart = false;
        std::string name;
        /// The version people know it by, such as 1.0.2.
        std::string displayVersion;
        std::shared_ptr<Image> icon;
    };

    struct IndexedFile {
        /// Of a game file, or for the folder of an extracted game, of the two files that say what it is. If that changes, it's read again.
        i64 size = 0;
        bool extracted = false;
        std::string problem;
        std::vector<IndexedTitle> titles;
    };

    /// One of the copies of a title there may be among the game files.
    struct IndexedCopy {
        const std::string* path = nullptr;
        const IndexedFile* file = nullptr;
        const IndexedTitle* title = nullptr;
    };

    /// What's under the folders the list of game files looks in, so that it can pick up where it left off after making way for the other list.
    struct FoundGames {
        bool walked = false;
        std::vector<std::pair<std::string, i64>> files;
        std::vector<std::pair<std::string, i64>> extracted;
    };

    /// A game file's title, opened only once it's something to do something with.
    struct OpenedJob {
        std::shared_ptr<TitleSet> set;
        const TitleJob* job = nullptr;
    };

    /// A game file's row with its DLC: the game first, then each DLC.
    struct OpenedJobs {
        std::shared_ptr<TitleSet> set;
        std::vector<const TitleJob*> jobs;
    };

    /// What a game comes to while it's being worked on, for working out what fits.
    struct RoomPlan {
        std::vector<std::string> order;
        std::vector<std::string> wontFit;
        i64 mostNeeded = 0;
        std::string reason;
    };

    struct LoadedGame;
    class Sink;

    // Starting up, the log, the threads and updating (app.cpp)
    void logLine(const std::string& text);
    void removeOldLogs();
    void openLog();
    void logFolder(const std::string& folder);
    void noteUnexpected(const std::string& what);
    void loadKeys();
    void lookForKeysAgain();
    void startingUp();
    void runInBackground(JobKind kind, std::function<void()> work);
    void quietenWorker();
    void stopAnalyst();
    void checkScanStop();
    void startAnalyzing();
    void startWhatsDue();
    void startUpdateCheck(bool byHand);
    void checkForUpdate(bool byHand);

    // What's kept between runs (app_saved.cpp)
    std::string readSaved(const char* name);
    void saveTwice(const char* name, const char* what, const std::string& text);
    void noteSaveProblem(const std::string& what, const std::string& why);
    void loadSettings();
    void saveSettings();
    ConvertOptions convertOptions() const;
    std::string nspFolder() const;
    std::string extractFolder() const;
    std::string cacheKey(const Row& row) const;
    void loadCache();
    void saveCacheLocked();
    void saveCacheSoonLocked();
    void rememberEstimateLocked(const Row& row);
    void rememberFailureLocked(const Row& row, const std::string& failure);
    bool recallEstimateLocked(Row& row);
    void loadHidden();
    void saveHiddenLocked();
    void loadList();
    void saveList();
    void loadFileIndex();
    void saveFileIndex();

    // The lists (app_lists.cpp)
    std::vector<std::string> fileRoots();
    std::vector<std::string> pcRoots();
    std::string whereIs(const std::string& path);
    void forgetFileScan();
    void updateFreeSpace();
    std::shared_ptr<Image> listIconFrom(const Bytes& jpeg);
    void bringIn(Row row, std::map<std::string, Row>* previous);
    std::string fingerprintOf(const InstalledGame& game) const;
    Row rowFor(const InstalledGame& game);
    void applyContents(Row& row, const InstalledGame& game);
    void scanInstalled();
    FoundGames lookForGames(const std::vector<std::string>& roots);
    IndexedFile readIndexEntry(const std::string& path, i64 size, bool extracted);
    bool fromPc(const std::string& path, IndexedFile& entry);
    void showWhatsStillThere(const std::vector<std::string>& roots);
    void scanFiles();
    void logWhatsDoneLocked();
    std::vector<std::string> indexedPathsLocked(const std::vector<std::string>& roots);
    void showIndexRowsLocked(const std::vector<std::string>& paths, bool ready);
    std::vector<Row> rowsFromIndexLocked(const std::vector<std::string>& paths);
    int placeRank(const std::string& path);
    IndexedCopy newestCopy(std::vector<IndexedCopy> copies, const char* kind, std::vector<std::string>& notes);
    bool chooseCopy(const std::vector<IndexedCopy>& all, const std::vector<IndexedCopy>& updatesFor, const char* kind, std::vector<std::string>& notes, IndexedCopy& main, IndexedCopy& update);
    Row rowForExtracted(const std::string& path, const IndexedTitle& title, bool done);
    Row rowForGame(const IndexedCopy& main, const IndexedCopy* update, const std::vector<IndexedCopy>& dlcs, std::vector<std::string> notes, bool compressed);
    OpenedJob openJob(const Row& row);
    OpenedJobs openJobs(const Row& row);
    void closeOpenJob();
    ExtractedGame openExtracted(const std::string& path);
    void judgeLocked(Row& row, const ConvertOptions& options);
    void judgeAllLocked();
    void sortRowsLocked();
    Row* findLocked(const std::string& key);
    /// Compressed by now, or found to be already.
    bool isCompressed(const Row& row) const;
    bool installedCompressedLocked(const Row& row) const;
    bool isDoneLocked(const Row& row) const;
    bool compressedToInstallLocked(const Row& row) const;
    bool hiddenAsDoneLocked(const Row& row) const;
    bool leftOffListLocked(const Row& row) const;
    int doneHiddenLocked(int tab) const;
    std::vector<Row*> visibleRowsLocked(int tab);
    std::vector<std::string> keysToAnalyzeLocked(int tab);
    /// Compressing a game file installs it, rather than saving an NSP.
    bool installsGameFiles() const { return _console && _settings.installFiles; }

    // The work (app_work.cpp)
    std::unique_ptr<LoadedGame> loadGame(u64 application);
    SizeEstimate estimateJob(const TitleJob& job, const ConvertOptions& options);
    bool estimateOnPc(const Row& row, SizeEstimate& estimate);
    SizeEstimate estimateRow(const Row& row);
    void analyze(const std::vector<std::string>& keys);
    void needRoom(Storage storage, i64 output);
    void needRoomIn(const std::string& folder, i64 output);
    bool goesOnConsole(const Row& row) const;
    bool replacesInstalled() const { return _settings.replaceInstalled; }
    Storage installTarget() const { return _settings.installToConsole ? Storage::Nand : Storage::Sd; }
    RoomPlan planRoom();
    std::vector<const char*> stepsFor(const Row& row) const;
    void workPlannedFor(const Row& row, JobKind kind, i64 (&planned)[2]) const;
    void prepareJob(const std::vector<std::string>& keys, JobKind kind);
    std::shared_ptr<Image> iconForJob(const Row& row);
    void loadBigIcon(const Row& row);
    void startGame(const Row& row, const std::vector<const char*>& steps);
    void endGameLocked();
    void noteWorkLocked(const char* label, i64 done);
    double overallLocked(double* secondsLeft);
    void noteStepLocked(const char* label);
    void finishJob(const std::string& title, const std::string& detail);
    InstalledGame whatsInstalled(u64 application, u32 versionComing, bool wholeGame, u64 titleId);
    Replacer replacer();
    bool consoleReadsWhatsMade(const std::vector<std::string>& keys);
    void compress(const std::vector<std::string>& keys);
    std::string compressSummary(double seconds);
    void compressOne(const Row& row, ResultLine& result, bool& leftOut);
    void compressInstalled(const Row& row, const SizeEstimate& estimate, const ConvertOptions& options, ResultLine& result);
    void buildExtracted(const Row& row, const SizeEstimate& estimate, const ConvertOptions& options, ResultLine& result);
    void compressFiles(const Row& row, const SizeEstimate& estimate, const ConvertOptions& options, ResultLine& result);
    void deleteInstalledFiles(const std::vector<std::string>& installed, std::string& detail);
    static ResultLine resultFor(const Row& row);
    void workThrough(const std::vector<std::string>& keys, const char* whenItFails, const std::function<void(const Row&, ResultLine&)>& work, const std::function<std::string(int, double)>& summary);
    void forEachTitle(const Row& row, const std::function<void(const TitleJob&, const TitleSet&)>& work);
    void extract(const std::vector<std::string>& keys);
    void check(const std::vector<std::string>& keys);
    void remove(const std::vector<std::string>& keys);
    void uninstall(const Row& row);
    void deleteFolder(const std::string& path);

    // What people can do (app_actions.cpp)
    void showMessage(const std::string& title, const std::string& text);
    void showToast(const std::string& text);
    void showAbout();
    void toggleRow(int index);
    void tickAll(bool ticked);
    void tickWorthIt();
    int tickActionCount() const;
    bool tickActionChangesLocked(TickAction action);
    TickAction nextTickActionLocked();
    static const char* tickActionLabel(TickAction action);
    void cycleTicks();
    void onlyTheSelected(std::function<void()> action);
    void restoreTicks();
    std::vector<std::string> tickedGames();
    void setTab(int tab);
    std::string sortDescription();
    void changeSort(int direction);
    void hideFocused();
    void showHidden();
    void setHideDone(bool hide);
    void lookElsewhere();
    void chooseFilesFolder();
    void toggleSource(bool& source);
    void analyzeAgain();
    void refreshList();
    void stopAnalyzing();
    bool stopAsked() const;
    void doWhenFree(std::function<void()> action);
    bool canActNow();
    void confirmTicked();
    void pressCompress();
    void pressInstall();
    void pressQuit();
    void beginJob(JobKind kind, const std::vector<std::string>& keys, std::function<void()> work);
    void beginCompress(const std::vector<std::string>& keys);
    void beginCheck();
    void beginExtract();
    bool askAboutGamesWithoutUpdates();
    void askToCompress();
    void askToStop();
    /// What's selected, or everything that's ticked.
    void askToRemove(bool allTicked);
    MenuFacts menuFacts();
    void openMenu(int page = MenuMain);
    void browse(const std::string& title, const std::string& start, const std::string& useDefaultLabel, std::function<void(const std::string&)> choose);
    /// By value, as what it's given is usually one of the entries it's about to replace.
    void browseTo(std::string path);
    void browseInto(int index);
    void browseUp();
    void browseChoose(std::string path);
    void browseUse();
    void browseUseDefault();
    void closeBrowser();
    std::string shownFolder(const std::string& path, const char* otherwise);
    OptionItem installedGamesOption(const char* section);
    OptionItem gameFilesOption(const char* section, const char* help);
    OptionItem nspFolderOption(const char* section);
    OptionItem minimumSavingOption(const char* section);
    OptionItem autoUpdateOption(const char* section);
    void compressionChanged();
    void buildOptions();
    void buildWelcome();
    void finishWelcome();
    const char* verb() const { return _settings.compress ? "压缩" : "重打包"; }
    const char* verbing() const { return _settings.compress ? "正在压缩" : "正在重打包"; }
    const char* verbed() const { return _settings.compress ? "压缩" : "重打包"; }

    // Input (app_input.cpp)
    void showWhatsWaiting();
    void catchUpWithDrives();
    void catchUpWithPcs();
    void updateMain(u32 pressed, const InputState& input, double dt);
    void updateOptionItem(OptionItem& item, u32 pressed, const InputState& input, double dt);
    void updateOptions(u32 pressed, const InputState& input, double dt);
    void updateWelcome(u32 pressed, const InputState& input, double dt);
    void updateResults(u32 pressed, const InputState& input, double dt);
    void updateBrowse(u32 pressed, const InputState& input, double dt);
    void updateDialog(u32 pressed);
    void updateMenu(u32 pressed, const InputState& input, double dt);
    bool repeated(u32 held, u32 button, double dt);
    void handleTouch(const InputState& input);
    void keepFocusVisible();
    void animate(double dt);
    /// Where a selection should be drawn on its way to a row, the same way on every list. Each list has its own number.
    double glide(int list, double target);
    /// The same, for what's scrolling.
    double glideTowards(double& shown, double target);

    // Drawing (app_draw.cpp)
    void drawMain(Canvas& c);
    void drawTopBar(Canvas& c);
    void drawStorage(Canvas& c, int x, int width, const char* label, i64 free, i64 total);
    void drawColumns(Canvas& c, const std::vector<Row*>& rows);
    void drawRow(Canvas& c, const Row& row, int y);
    void drawEmpty(Canvas& c, const std::string& title, const std::string& detail, bool spinner);
    void drawEmptyList(Canvas& c);
    void drawFooter(Canvas& c, const std::vector<Row*>& rows);
    void drawHints(Canvas& c, const std::vector<Hint>& hints);
    void drawButtonGlyph(Canvas& c, float cx, float cy, const char* name, float radius, Color fill, Color ink);
    void drawOptions(Canvas& c);
    void drawWelcome(Canvas& c);
    void drawChangeArrows(Canvas& c, float left, float right, float mid, Color ink);
    void drawJob(Canvas& c);
    void drawResults(Canvas& c);
    void drawNoKeys(Canvas& c);
    void drawBrowse(Canvas& c);
    void drawDialog(Canvas& c);
    void drawMenu(Canvas& c);
    void drawMenuBehind(Canvas& c);
    bool drawOverMenuBehind(Canvas& c);
    void drawToast(Canvas& c);
    void drawTick(Canvas& c, float cx, float cy, float radius, bool ticked, bool enabled);
    void drawTile(Canvas& c, const std::shared_ptr<Image>& icon, Color tile, const std::string& name, int x, int y, int size, int radius);
    void drawSpinner(Canvas& c, float cx, float cy, float radius);
    void drawBar(Canvas& c, Rect r, double fraction, Color fill);

    Platform& _platform;
    ConsoleApi* _console = nullptr;
    Font _font;
    Keyset _keyset;
    std::string _keysPath;
    std::string _keysProblem;
    bool _dark = true;
    bool _memoryLimited = false;
    /// Being opened as an applet has been said, as it is once each time.
    bool _appletSaid = false;
    std::vector<Drive> _drives;
    bool _drivesChanged = false;

    // The lists, and what's kept about them
    sys::Mutex _mutex;
    std::vector<Row> _rows;
    std::map<std::string, std::string> _cache;
    bool _cacheChanged = false;
    double _cacheSavedAt = 0;
    std::set<std::string> _hidden;
    std::map<std::string, IndexedFile> _fileIndex;
    bool _fileIndexChanged = false;
    FoundGames _found;
    bool _listChanged = false;
    sys::Mutex _listSaving;
    sys::Mutex _indexSaving;
    bool _sortedByGain[TabCount] = {false, false, false};
    std::shared_ptr<TitleSet> _openSet;
    std::string _openSetFiles;
    std::string _openExtractedPath;
    std::shared_ptr<ExtractedGame> _openExtracted;
    /// PCs sharing their games, which come and go: when that was last asked, and which the list of game files last looked in.
    static constexpr double PcCheckSeconds = 1;
    bool _pcsChanged = false;
    double _lastPcCheck = 0;
    sys::Mutex _pcsMutex;
    std::vector<std::string> _pcsLookedIn;
    Settings _settings;

    // The threads
    sys::Thread _worker;
    std::atomic<bool> _workerRunning{false};
    std::atomic<JobKind> _jobKind{JobKind::None};
    std::unique_ptr<JobSink> _sink;
    /// Analyzing goes on beside whatever a list needs to come up, which is mostly waiting for the system's file service. Everything else waits for it to stop.
    sys::Thread _analyst;
    std::atomic<bool> _analystRunning{false};
    std::atomic<int> _analyzingTab{-1};
    int _analysisIndex = 0;
    int _analysisCount = 0;
    std::string _analysisTitle;
    bool _autoAnalyze[TabCount] = {true, true, false};
    /// A list coming up gives way to the one on the screen, without taking analyzing with it.
    std::atomic<bool> _scanStop{false};
    std::atomic<bool> _scanned[TabCount] = {{false}, {false}, {true}};
    bool _scanWanted[TabCount] = {false, false, false};
    std::atomic<int> _scanningTab{-1};
    std::atomic<bool> _rescanInstalled{false};
    /// What's waiting for analyzing to stop, or for a list to come up.
    std::function<void()> _afterWork;
    /// Nothing should sleep while something's being done.
    bool _working = false;

    // The job, and how far along it is. Whatever changes during a job is guarded by _mutex.
    std::atomic<bool> _resultsReady{false};
    std::atomic<bool> _stopAfterGame{false};
    /// What's ticked gets installed this time, whatever the options say to do with it.
    bool _installNow = false;
    /// Games with no update have been asked about, and are to be repacked anyway.
    bool _keepGamesWithoutUpdates = false;
    std::set<Storage> _consoleChecked;
    int _jobIndex = 0;
    int _jobCount = 0;
    std::string _jobTitle;
    std::shared_ptr<Image> _jobIcon;
    Color _jobTile;
    std::vector<const char*> _jobSteps;
    int _jobStep = -1;
    std::string _jobLabel;
    i64 _jobDone = 0;
    i64 _jobFilesDone = 0;
    i64 _jobFilesTotal = 0;
    i64 _jobTotal = 0;
    double _jobStarted = 0;
    i64 _jobFreed = 0;
    /// How much there is to get through and how quickly it's going: what gets compressed or copied, and what gets read back afterwards.
    struct Work {
        i64 planned = 0;
        i64 done = 0;
        double seconds = 0;
    };
    Work _work[2];
    i64 _gamePlanned[2] = {0, 0};
    i64 _gameDoneBefore[2] = {0, 0};
    int _gamesFinished = 0;
    int _workKindNow = -1;
    std::string _workLabel;
    i64 _workLastDone = 0;
    double _workLastAt = 0;
    double _overallShown = 0;
    double _jobRate = 0;
    double _jobRateAt = 0;
    i64 _jobRateDone = 0;
    std::vector<ResultLine> _results;
    std::string _resultsTitle;
    std::string _resultsDetail;
    i64 _freeSd = -1, _totalSd = 0, _freeNand = -1, _totalNand = 0, _freeUsb = -1, _totalUsb = 0;
    /// What went wrong that nothing was expecting, which gets shown, as there may be no log to look at.
    std::string _unexpected;

    // Updating
    sys::Thread _updateThread;
    std::atomic<bool> _updating{false};
    std::atomic<bool> _updateStop{false};
    std::string _updateState;
    std::string _updateNoticeTitle;
    std::string _updateNoticeText;
    std::string _updatedProgram;

    // The log
    sys::Mutex _logMutex;
    std::unique_ptr<sys::File> _log;
    std::string _logPath;
    i64 _logPosition = 0;
    double _logClosedAt = 0;
    /// What's said before there's any knowing whether there's to be a log, which is in the settings.
    std::vector<std::string> _earlyLog;
    bool _logDecided = false;
    /// Why the log can't be written, which gets shown, as there's no log to look at.
    std::string _logProblem;
    bool _closedDown = false;
    std::string _saveProblem;
    bool _saveProblemShown = false;
    /// What happened last time that's worth saying, such as a job a power cut interrupted.
    std::vector<std::string> _notes;

    // What's on the screen
    Screen _screen = Screen::Main;
    int _tab = TabInstalled;
    int _focus[TabCount] = {0, 0, 0};
    int _focusSeen[TabCount] = {0, 0, 0};
    double _scroll[TabCount] = {0, 0, 0};
    int _tickStep[TabCount] = {0, 0, 0};
    /// What was ticked before doing something to just the selected game, to go back to afterwards.
    std::vector<std::pair<std::string, std::pair<bool, bool>>> _savedTicks;
    double _focusShown = 0;
    double _scrollShown = 0;
    double _tabShown = 0;
    /// What's behind the + menu, as drawing it again for every frame the menu moves is far more than the menu itself.
    std::vector<u32> _menuBehind;
    int _menuBehindPage = -1;
    int _menuBehindItems = -1;
    std::string _menuBehindStatus;
    int _glideList = -1;
    double _glideShown = 0;
    double _glideStep = 1;
    std::vector<OptionItem> _options;
    int _optionFocus = 0;
    int _lastOptionFocus = 0;
    double _optionScroll = 0;
    double _optionScrollShown = 0;
    bool _rebuildOptions = false;
    std::vector<OptionItem> _welcome;
    int _welcomeFocus = 0;
    int _resultTop = 0;
    std::unique_ptr<Dialog> _dialog;
    std::unique_ptr<Menu> _menu;
    std::unique_ptr<Browser> _browser;
    Screen _browseFrom = Screen::Main;
    std::vector<Hit> _hits;
    std::string _toast;
    double _toastUntil = 0;
    double _time = 0;
    double _lastWorkDraw = 0;
    std::atomic<bool> _dirty{true};
    bool _quit = false;

    // The buttons and the touch screen
    u32 _lastHeld = 0;
    double _repeatTimer = 0;
    u32 _repeatButton = 0;
    bool _touchWasDown = false;
    int _touchStartX = 0, _touchStartY = 0, _touchLastY = 0;
    bool _touchDragged = false;
};

}  // namespace hbp
