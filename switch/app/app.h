// The Switch version: lists of what's on the console, and of game files wherever they're looked for, how much each would gain, and doing
// something about it. It knows nothing of the console itself: everything it needs comes through Platform and ConsoleApi, so all of it runs
// on a PC too.
#pragma once

#include <deque>
#include <map>
#include <optional>
#include <set>

#include "../core/console.h"
#include "canvas.h"

namespace hbp {

constexpr const char* AppVersion = "1.5.1";

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

/// A game the console started, from its play history: which, and when it started and ended, in seconds since 1970, as time() has them.
struct GamePlayed {
    /// The game's title ID, as the history has it, which for a game of several programs can differ in its last digits.
    u64 application = 0;
    long long started = 0;
    /// 0 if it hasn't ended, or there's no telling when, as when the console was turned off with it running.
    long long ended = 0;
};

/// Pictures inside games (app_picture.cpp): false for anything that isn't one these can show.
bool decodePng(const Bytes& data, Image& out);
bool decodeBmp(const Bytes& data, Image& out);
bool jpegSize(const Bytes& data, int& width, int& height);

class Platform {
public:
    virtual ~Platform() = default;
    /// Null if the console's services can't be had, with the reason from consoleProblem.
    virtual ConsoleApi* console() = 0;
    virtual std::string consoleProblem() = 0;
    virtual void loadFonts(Font& font) = 0;
    virtual bool decodeIcon(const Bytes& jpeg, Image& out) = 0;
    /// A JPEG of any size, for Look inside: the console's own decoder. PNG and BMP are decoded by the app itself (app_picture.cpp).
    virtual bool decodeJpeg(const Bytes&, Image&) { return false; }
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
    /// Where homebrew apps each have a folder of their own, which is only looked through near the top: what's deeper down is their own data,
    /// caches and scripts, thousands of folders of it on a card with a few apps, and never game files.
    virtual std::vector<std::string> appFolders() = 0;
    virtual std::vector<std::string> keyLocations() = 0;
    /// Where the custom firmware keeps its reports of what crashed, for a report of a problem.
    virtual std::vector<std::string> crashReportFolders() = 0;
    /// The games the console started after then, oldest first, from its play history. Empty if the history can't be read.
    virtual std::vector<GamePlayed> gamesPlayedSince(long long since) = 0;
    /// Nothing should sleep or dim while something's being done.
    virtual void setWorking(bool working, bool boost) = 0;
    /// Negative if there's no telling.
    virtual int batteryPercent() = 0;
    virtual bool isCharging() = 0;
    /// Opened in a way that leaves little memory to work with, which a large solid NSZ may not fit in.
    virtual bool memoryIsLimited() = 0;
    /// The firmware has to be new enough to read the compression this stores games with.
    virtual bool firmwareReadsCompression() = 0;
    /// The console's system version, written as a game's record writes the version it needs: 12.0.0 is 0x30000000. Zero if there's no telling.
    virtual u32 systemVersion() = 0;
    /// A line for the log about what this is running on.
    virtual std::string aboutSystem() = 0;

    /// Whether the console is connected to the internet at all.
    virtual bool isOnline() = 0;
    /// What's at an address, as long as it isn't more than the limit and keepGoing stays true. Throws if it can't be had.
    virtual Bytes download(const std::string& url, size_t limit, const std::function<bool()>& keepGoing) = 0;
    /// Sends JSON to an address, and what came back with its status. Throws if nothing came back at all.
    virtual std::pair<long, Bytes> post(const std::string& url, const std::string& json, const std::function<bool()>& keepGoing) = 0;
    /// What the person types on the system's keyboard, with a word on what to type. Nothing if they cancel it; a keyboard that can't be shown
    /// counts as nothing typed.
    virtual std::optional<std::string> typeText(const std::string& guide, size_t most) = 0;
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
    /// Called by start() as soon as there's a font and a theme to draw with, before the lists are read, so the screen needn't stay black until then.
    std::function<void()> whenItCanBeDrawn;
    /// What's on the screen while the rest of start() goes on.
    void drawOpening(Canvas& canvas);
    void update(const InputState& input, double dt);
    void draw(Canvas& canvas);
    /// Whether something on screen has changed since the last draw.
    bool needsDrawing();
    bool wantsToQuit() const { return _quit; }
    bool showingWelcome() const { return _screen == Screen::Welcome; }
    bool showingOverview() const { return _screen == Screen::Main && _overview && !_dialog && !_menu; }
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
        i64 now = 0, after = 0;
        /// Its second line and Saved column as the view shown has them.
        std::string second, saved;
    };
    /// Every game on either list, hidden or not, with what its row says, for the checks that compare one run of the app with another.
    struct FullRow {
        int tab = 0;
        std::string key;
        std::string name;
        bool visible = false;
        /// Which of the views it's in, whichever is shown.
        bool inToDo = false, inDone = false;
        bool ticked = false;
        bool byHand = false;
        bool blocked = false;
        std::string state;
        /// What the row says, as drawn: the line under the name, the sizes, and anything said in place of them.
        std::string second, now, after, saved, share, status;
        bool hasEstimate = false;
        bool alreadyCompressed = false;
        /// A first look can't vouch for it, and whether what it has was measured.
        bool notSure = false, measured = false;
        /// Analysis hasn't read it in full yet, and would.
        bool unmeasured = false;
        /// A copy an older version made, looked at again, and what compressing it freed.
        bool olderCopy = false;
        i64 freedBytes = 0;
        i64 nowBytes = 0, afterBytes = 0, marginBytes = 0, savedBytes = 0;
        std::string mainFile, updateFile, extracted;
        std::vector<std::string> dlcFiles;
        bool hasUpdate = false;
        bool overInstalled = false;
    };
    struct Snapshot {
        std::string screen;
        int tab = 0;
        std::string label;
        bool analyzing = false;
        /// What's being analyzed is being read in full, for a thorough analysis.
        bool measuring = false;
        /// What the footer says about analysis, as "Measuring X, 1 of 2". Empty when nothing is analyzed.
        std::string analysisLine;
        /// Analysis reads every game in full by itself, as it does to begin with.
        bool thorough = false;
        bool scanning = false;
        std::string dialog;
        std::string dialogText;
        std::string toast;
        std::string sortedBy;
        std::string tickHint;
        std::string actionWord;
        std::string resultsTitle;
        std::string resultsDetail;
        std::string resultsAdvice;
        std::vector<std::string> results;
        std::vector<RowView> rows;
        int compressedHidden = 0;
        double overall = 0;
        std::string browsing;
        std::vector<std::string> browseEntries;
        /// Details and Look inside, as they're shown.
        bool insideReady = false;
        std::string insideProblem;
        std::string detailsState;
        /// Each fact as "section: label: value (note)".
        std::vector<std::string> details;
        std::vector<std::string> detailsNotes;
        std::string insidePath;
        /// What's in the folder shown, each as "name | kind | size".
        std::vector<std::string> insideEntries;
        int insideFocus = 0;
        bool insidePreviewReady = false;
        std::string previewKind;
        std::vector<std::string> previewChips;
        std::vector<std::string> previewLines;
        int pictureW = 0, pictureH = 0;
        bool insideFull = false;
        bool insideSaving = false;
        std::string insideSavedTo;
        std::vector<std::string> options;
        int optionFocus = 0;
        std::vector<std::string> menu;
        int menuFocus = 0;
        std::vector<std::string> welcome;
        int welcomeFocus = 0;
        /// What explains something but was drawn cut short on the last frame, which should never happen.
        std::vector<std::string> cut;
        /// Anything else shortened to fit on the last frame, with an ellipsis: names, figures, labels.
        std::vector<std::string> fitCut;
        /// Words that ran into the bar beside them on the last frame, which should never happen.
        std::vector<std::string> crowded;
        /// The list's column headings on the last frame, left to right.
        std::vector<std::string> headings;
        /// Games drawn with their letters on the last frame though they have an icon, which should never happen either.
        std::vector<std::string> lettersForIcons;
        std::string updateState;
        /// Both lists in full, in the order they're in, and what X is called on each.
        std::vector<FullRow> all;
        std::string actionWords[2];
        /// Each list is only read once it's gone to.
        bool scanned[2] = {false, false};
        /// What each list shows: To do, Done or Everything.
        std::string views[2];
        /// The overview, when it's what's on the screen: its headline, and each of its lines with the list it opens.
        bool overview = false;
        std::string headline;
        struct Line {
            int tab = 0;
            /// Looking, ToDo, ToInstall, Done, Nothing, NotSure, WontStart or Developer.
            std::string kind, text, sub;
            i64 saved = 0;
            /// How many of its games it shows, the ones saving the most.
            int shown = 0;
            /// How many ticked games its button does, and how many of those are measured first.
            int count = 0, notSure = 0;
        };
        std::vector<Line> overviewLines;
        /// Where the pointer stops on the overview, in order: each line, by its place in overviewLines, and each game under a line, by its
        /// name, with where each one is on the screen.
        struct Stop {
            int tab = 0;
            int line = 0;
            std::string game;
            int x = 0, y = 0, w = 0, h = 0;
            /// Line, Game, Act, Choose or Sources, and what a button says.
            std::string kind, label;
        };
        std::vector<Stop> overviewStops;
        /// Which of the overview's stops the pointer is on.
        int overviewFocus = 0;
        /// Which of the rows has the focus, on a list.
        int focus = -1;
        /// Free space now and what compressing what's ticked would free, on each place games go.
        struct Meter {
            std::string label;
            i64 free = -1, frees = 0;
        };
        std::vector<Meter> meters;
    };
    Snapshot snapshot();
    double lastDrawMs() const { return _lastDrawMs; }

private:
    /// Settings is a screen of its own, opened from the + menu, so it's only ever come to on purpose.
    enum class Screen { Main, Job, Results, NoKeys, Browse, Welcome, Settings, Details, Inside };
    enum class RowState { Ready, Working, Done, Failed };
    enum class JobKind { None, Starting, Scanning, Compressing, Extracting, Checking, Removing, Fixing };
    enum class TickAction { All, None, WorthIt };
    /// Which games a list shows. It starts on what's still to do, and the - button goes through the others until the app closes.
    enum class View { ToDo, Done, Everything };
    enum { MenuMain, MenuThisGame, MenuTicked, MenuList, MenuSources, MenuHelp, MenuResults, MenuInside };

    static constexpr int TabInstalled = 0;
    /// Game files and the folders of extracted games, wherever they're looked for: they're all games that aren't installed.
    static constexpr int TabFiles = 1;
    static constexpr int TabCount = 2;

    struct Row {
        /// When its tick was last changed by hand, so the tick pops, and its bar as drawn, which glides to new figures.
        double tickedAt = -1;
        mutable double barShown = -1;
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
        /// An installed game that won't start, as a newer official update went in over a copy this rebuilt, until compressing makes them one.
        /// That's always worth doing, whatever it saves.
        bool updateOverCopy = false;
        /// An installed copy this rebuilt says something on its record that stops it starting, which Fix takes off: before version 1.5, that
        /// it needs its update; or, made in place of a game an installer had let start on an older system, the newer system version its files
        /// ask for.
        bool markedIncomplete = false;
        /// What a copy's record asks of the system when that's newer than this console, which is why it won't start. Zero otherwise.
        u32 asksSystem = 0;
        /// An installed game has DLC installed with it. A game file's DLC is in dlcFiles.
        bool hasDlc = false;
        /// A file this compressed, which repacking with compression off only makes bigger.
        bool compressedFile = false;
        /// With Game and DLC in one file, while game files are saved as NSPs: a copy this tool made whose DLC here isn't all in its file, or DLC
        /// without their game that no one file holds. Saving makes their file again with all of them. The usual rules say whether that's worth
        /// it, and until then it doesn't count as compressed.
        bool dlcNotInFile = false;
        /// With Game and DLC in one file off, while game files are saved as NSPs: beside this tool's copy of its game, its DLC with no file of
        /// their own, such as one only inside a merged original. Saving gives each one, whatever that saves, and until then the row is to do.
        std::vector<u64> dlcWithoutFile;
        /// It's known to be there as described, rather than only remembered from last time.
        bool ready = false;
        bool ticked = true;
        bool byHand = false;
        bool hasEstimate = false;
        SizeEstimate estimate;
        /// Why it isn't worth compressing, which settles it: it's under Done, saying so.
        std::string belowMinimum;
        /// That's only the minimum saving: compressing it saves something, less than that.
        bool underMinimum = false;
        /// A first look can't vouch for it, and only reading all of it can say: Measure does that, writing nothing. It isn't ticked until then,
        /// as that read takes a while and usually finds too little. See SavingRule::notSure.
        bool notSure = false;
        /// Measured by a version from before games under 1 GB were measured exactly, which they are once more, in turn.
        bool measureAgain = false;
        /// Where it stood in the list when its answer was forgotten to work it out again, which it keeps until it has a new one, so the list
        /// stays in order around it. A rank of -1 keeps nothing.
        struct Kept {
            int rank = -1;
            double fraction = 0;
            bool done = false;
        } kept;
        /// What it's sure to save, as lists and totals count it: nothing for one that isn't sure yet.
        i64 sureSaved() const { return hasEstimate && !notSure ? estimate.sureSavedBytes() : 0; }
        /// What measuring it reads through: its game data, as the first look found it, or the whole of it if that's not known.
        i64 measureBytes() const { return hasEstimate && estimate.romFsBytes > 0 ? estimate.romFsBytes : now; }
        /// Measured, it's measured exactly: every block compressed.
        bool measuredExactly() const { return now < MeasureExactlyBelow; }
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
        /// An update file, for a game that's installed, which goes in made part of what's installed, and the game's other update files,
        /// one of which may be the update that's in what's installed, which says what it kept of the original.
        bool overInstalled = false;
        std::vector<std::string> otherUpdates;
        /// What keeping its solid NSZs' streams aside can take, as a game file read with its update, or an update read over the installed game,
        /// is read out of order: the largest of each file's (TitleSummary::solidBytes), added up. Zero with nothing packed solid, or nothing
        /// read out of order.
        i64 solidBytes = 0;
    };

    struct Settings {
        int minSaving = 5;
        bool hideSmall = true;
        /// Only read now, from settings saved by versions with "隐藏已完成的游戏": off, the lists started on Everything, which they
        /// still do until a view has been chosen (view).
        bool hideCompressed = true;
        /// The view each list was last on, which it opens on again: To do, Done or Everything, or -1 for none chosen yet.
        int view[2] = {-1, -1};
        /// Where the list of game files looks: the SD card, USB drives, and PCs running hacbrewpacklz send on the network or the cable.
        bool fromSd = true;
        bool fromUsb = true;
        bool fromPcNetwork = true;
        bool fromPcCable = true;
        /// A game card that's in, whose games can be installed or saved like game files.
        bool fromGameCard = true;
        /// A PC on both the cable and the network is read over the network, rather than the cable, which is faster and steadier.
        bool wifiFirst = false;
        bool compress = true;
        int level = 9;
        /// What retail games use. Smaller only compresses worse, so it isn't a setting, and one an earlier version saved isn't gone by.
        int blockSize = 0x10000;
        bool replaceInstalled = true;
        bool installFiles = true;
        /// An NSP saved of a game holds its DLC too, after it in title ID order. Off, each DLC gets a file of its own.
        bool dlcInGameFile = true;
        /// Where a game that isn't installed yet goes. One that is stays where it is.
        bool installToConsole = false;
        bool deleteInstalled = false;
        bool boost = true;
        bool warned = false;
        int sort = 0;
        bool autoUpdate = true;
        /// Problems the app finds itself go to the developer by itself. Off until the person says so.
        bool sendProblems = false;
        /// Sending problems has been asked about: on the welcome, or the once, for whoever had the welcome before that was on it.
        bool problemsAsked = false;
        /// After its quick first look at each game, analysis goes on to read every game in full, writing nothing, while the app is open, the
        /// games it could change the answer for first, so every figure ends up exact, ticking the ones worth it. Off, games a first look
        /// can't vouch for stay unticked.
        bool thorough = true;
        /// Asked once, after files were first installed and kept, whether to delete them.
        bool deleteAsked = false;
        /// The welcome screen has been and gone.
        bool welcomed = false;
        /// The settings that rarely need changing are showing.
        bool moreSettings = false;
        /// Only when it's wanted, as it takes writing to the card.
        /// On to begin with, so that whatever goes wrong comes with what led up to it.
        bool writeLog = true;
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
        /// A web address to show as a QR code beside the words, for reading the rest on a phone.
        std::string qr;
        std::string qrCaption;
        /// The code's modules, worked out the first time it's drawn.
        std::vector<u8> qrModules;
        int qrSize = 0;
        /// Never the same for two questions, so what was kept from behind one is never shown behind another.
        u64 serial = nextSerial();

    private:
        static u64 nextSerial() {
            static std::atomic<u64> made{0};
            return ++made;
        }
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
        /// B goes back from a page to the one the menu was opened on, which is what that screen has: the results' own, Look inside's,
        /// or a list's.
        int page = 0;
        int root = 0;
    };
    struct MenuFacts;

    struct ResultLine {
        std::string key;
        std::string name;
        std::string outcome;
        RowState state = RowState::Done;
        std::string message;
        i64 before = 0, after = 0;
        std::shared_ptr<Image> icon;
        Color tile;
        /// Left out as it saves less than the minimum, rather than having nothing to gain at all.
        bool notWorthIt = false;
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

    /// One thing inside a game, as Look inside lists it: a folder, or a file with where its bytes are.
    struct InsideNode {
        std::string name;
        bool folder = false;
        /// A file's size, or all that a folder holds.
        i64 size = 0;
        int items = 0;
        int parent = -1;
        std::vector<int> children;
        /// Which of the game's parts a file's bytes are in, and where.
        int part = -1;
        i64 offset = 0;
        /// What it is, in a word: Folder, Text, Picture, Sound, Video, Archive, Font, Model, Program or Data.
        std::string kind;
    };
    struct DetailsLine {
        std::string label, value, note;
        /// What's good about it, in the accent colour, or what's wrong, in red.
        bool good = false, bad = false;
    };
    struct DetailsSection {
        std::string title;
        std::vector<DetailsLine> lines;
    };
    /// A game opened to look at: its details, and what it's made of, as folders. What's read from the game is read on a thread of its own, as a
    /// game on a PC or a game card can take a moment to open, and the screen never waits for it.
    struct Inside {
        std::string key;
        int tab = 0;
        std::string name, publisher, version;
        std::shared_ptr<Image> icon;
        Color tile;
        /// What's good to know about it, which About this game said before there were details: what stops it, what went wrong, and other
        /// copies of it.
        std::vector<std::string> notes;
        /// The way back from Look inside: to the details it was opened from, or straight to the list.
        bool fromDetails = false;
        // Read from the game: everything from here down to the preview is only touched by the reader until ready.
        bool ready = false;
        std::string problem;
        std::string stem;
        std::shared_ptr<TitleSet> set;
        std::vector<std::pair<std::string, SourcePtr>> parts;
        std::vector<InsideNode> nodes;
        i64 files = 0, bytes = 0;
        // Details: the one figure on the left, and the facts on the right, which go up and down when there's more than fits.
        std::string stateLabel, stateBig, stateSub, shareWord;
        double share = -1;
        std::vector<DetailsSection> sections;
        int detailsTop = 0, detailsMost = 0;
        double detailsShown = 0;
        // Looking inside: the folder shown, the pointer, and where it was in each folder above.
        int folder = 0;
        std::vector<int> focusAbove;
        int focus = 0, lastFocus = -1;
        double scroll = 0, scrollShown = 0;
        /// Found by name: a folder of its own, made up of what was found, shown in place of the one the pointer was in.
        std::vector<int> found;
        std::string finding;
        int foundFrom = -1;
        // What's under the pointer, beside the list and on the whole screen: its lines as text, its bytes as they are, a picture, or for a
        // folder, what it's made of. Each kind of file is shown its own way, so more kinds can be shown as they come.
        int previewOf = -1;
        bool previewReady = false;
        std::string previewKind;
        /// What's said about it in a few words, beside its name: its kind, its size, and how many lines or how big a picture.
        std::vector<std::string> chips;
        std::vector<std::string> lines;
        int totalLines = 0;
        Bytes raw;
        bool rawShown = false;
        /// The picture made to fit beside the list, and the whole screen.
        std::shared_ptr<Image> picture, pictureFull;
        /// Its icon as big as the details show it, and as small as the top of Look inside does, from the game's own picture once it's read.
        std::shared_ptr<Image> iconBig, iconSmall;
        struct KindCount {
            std::string kind;
            int files = 0;
            i64 bytes = 0;
        };
        std::vector<KindCount> breakdown;
        bool full = false;
        int fullTop = 0;
        bool asBytes = false;
        /// When the pointer last moved on, so what was shown before stays a moment while the next is read, rather than a spinner flashing.
        double wantSince = 0;
        /// A folder gone into slides in from the right, and one gone back up to from the left.
        double slideSince = -1;
        int slideFrom = 0;
        // Saving to the SD card, how far it's got.
        bool saving = false;
        i64 savedFiles = 0, savingFiles = 0, savedBytes = 0, savingBytes = 0;
        std::string savedTo;
        /// How many lines the whole screen goes through: the text's, or the bytes', sixteen a line.
        int fullLines() const { return rawShown ? (int)((raw.size() + 15) / 16) : (int)lines.size(); }
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
        /// This tool compressed it, which its headers say, whatever the file is called, and what the game came to before, if the copy says.
        bool madeHere = false;
        i64 originalBytes = 0;
        /// Its largest stream packed solid, as an NSZ's solid compression packs it: what keeping it aside takes, if an update reads it.
        i64 solidBytes = 0;
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
    /// Files a job has saved, for the list of game files.
    std::vector<std::string> _savedFiles;
    void noteSaved(const std::string& path);
    void addSavedFiles();

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
        /// What's needed counts a temporary copy of a solid NSZ, without which it's hard to see why.
        bool countsCopy = false;
    };

    struct LoadedGame;
    class Sink;

    // Starting up, the log, the threads and updating (app.cpp)
    void logLine(const std::string& text);
    /// From the start on, running out of memory is noted as it happens, so the log can say how it was.
    void watchMemory();
    /// What the log says about the memory after it ran out, where that can be known.
    std::string memoryNote();
    void removeOldLogs();
    void openLog();
    void nameLogLocked();
    void logFolder(const std::string& folder);
    void noteUnexpected(const std::string& what);
    void loadKeys();
    void lookForKeysAgain();
    void startingUp();
    void runInBackground(JobKind kind, std::function<void()> work);
    void quietenWorker();
    void stopAnalyst();
    void checkScanStop();
    bool lookingThroughThisList() const;
    void stopLooking();
    void startAnalyzing();
    void startWhatsDue();
    void startUpdateCheck(bool byHand);
    void checkForUpdate(bool byHand);

    // What's kept between runs (app_saved.cpp)
    std::string readSaved(const char* name);
    /// How many of the two copies were written.
    int saveTwice(const char* name, const char* what, const std::string& text);
    void noteSaveProblem(const std::string& what, const std::string& why);
    void loadSettings();
    void saveSettings();
    ConvertOptions convertOptions() const;
    std::string nspFolder() const;
    std::string extractFolder() const;
    std::string cacheKey(const Row& row) const;
    /// Where what's been found out about whether a game was rebuilt by this tool is remembered.
    std::string rebuiltKey(const std::string& fingerprint) const;
    void loadCache();
    void saveCacheLocked();
    void saveCacheSoonLocked();
    void rememberEstimateLocked(const Row& row);
    void rememberFailureLocked(const Row& row, const std::string& failure);
    bool stillGoodFromBefore(const Row& row, const std::string& answer) const;
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
    /// A game on the game card is on the list of game files as gamecard:<its ID>, and read through the console's records of the card.
    static bool isCardPath(const std::string& path);
    static std::string cardPath(u64 application);
    /// The game card's games, as files of that size to go on the list.
    std::vector<std::pair<std::string, i64>> gameCardFiles();
    /// What's on the game card of a game, without anything of it installed elsewhere.
    InstalledGame cardGame(u64 application);
    /// What a game file is opened as: the file, or the game card through the console.
    ArchivePtr archiveFor(const std::string& path);
    void forgetFileScan();
    void updateFreeSpace();
    std::shared_ptr<Image> listIconFrom(const Bytes& jpeg);
    void bringIn(Row row, std::map<std::string, Row>* previous);
    std::string fingerprintOf(const InstalledGame& game) const;
    Row rowFor(const InstalledGame& game);
    void applyContents(Row& row, const InstalledGame& game);
    void scanInstalled();
    /// A game's version as installed, the newest of the game and its update, to set against what the console wants of it.
    struct InstalledVersion {
        u64 application = 0;
        u32 version = 0;
    };
    void logVersions(const std::vector<InstalledVersion>& installed);
    FoundGames lookForGames(const std::vector<std::string>& roots);
    IndexedFile readIndexEntry(const std::string& path, i64 size, bool extracted);
    bool fromPc(const std::string& path, IndexedFile& entry);
    void showWhatsStillThere(const std::vector<std::string>& roots);
    void scanFiles();
    /// With one folder chosen, adds what this made of its games where NSPs are saved, elsewhere, to the paths found.
    void copiesInNspFolder(const std::vector<std::string>& roots, std::vector<std::string>& paths);
    void logWhatsDoneLocked();
    std::vector<std::string> indexedPathsLocked(const std::vector<std::string>& roots);
    void showIndexRowsLocked(const std::vector<std::string>& paths, bool ready);
    std::vector<Row> rowsFromIndexLocked(const std::vector<std::string>& paths);
    int placeRank(const std::string& path);
    IndexedCopy newestCopy(std::vector<IndexedCopy> copies, const char* kind, std::vector<std::string>& notes, const std::function<bool(const IndexedCopy&)>& first = nullptr);
    /// holdsAll, given with Game and DLC in one file while NSPs are saved, says whether a file holds every DLC of the game, at least as new.
    /// dlcWithoutFile: a DLC of the game has no file of its own.
    bool chooseCopy(const std::vector<IndexedCopy>& all, const std::vector<IndexedCopy>& updatesFor, const char* kind, std::vector<std::string>& notes, IndexedCopy& main, IndexedCopy& update,
                    const std::function<bool(const IndexedFile&)>& holdsAll, bool dlcWithoutFile = false);
    Row rowForExtracted(const std::string& path, const IndexedTitle& title, bool done);
    /// apart: how many of its DLC aren't in the file of a copy this tool made.
    Row rowForGame(const IndexedCopy& main, const IndexedCopy* update, const std::vector<IndexedCopy>& dlcs, std::vector<std::string> notes, bool compressed, int apart);
    Row rowForUpdate(const IndexedCopy& update, const Row& installed);
    OpenedJob openJobOverInstalled(const Row& row);
    OpenedJob openJob(const Row& row);
    OpenedJobs openJobs(const Row& row);
    void closeOpenJob();
    ExtractedGame openExtracted(const std::string& path);
    void judgeLocked(Row& row, const ConvertOptions& options);
    void judgeAllLocked();
    void judgeFilesOfLocked(u64 application);
    void installedGoneLocked(const std::set<u64>& applications);
    void sortRowsLocked();
    /// Where a game goes in a sort by gain: those with a figure, then those measured first, then those left out, then those with no answer yet.
    static int gainRank(const Row& row) { return !row.hasEstimate ? 3 : !row.belowMinimum.empty() ? 2 : row.notSure ? 1 : 0; }
    Row* findLocked(const std::string& key);
    /// Compressed by now, or found to be already.
    bool isCompressed(const Row& row) const;
    bool installedCompressedLocked(const Row& row) const;
    bool installedAsNewLocked(const Row& row) const;
    bool repackGainsNothingLocked(const Row& row) const;
    bool isDoneLocked(const Row& row) const;
    bool compressedToInstallLocked(const Row& row) const;
    bool inViewLocked(const Row& row, View view) const;
    bool leftOffListLocked(const Row& row) const;
    int doneHiddenLocked(int tab) const;
    std::vector<Row*> visibleRowsLocked(int tab);
    std::vector<std::string> keysToAnalyzeLocked(int tab);
    /// Compressing a game file installs it, rather than saving an NSP.
    bool installsGameFiles() const { return _console && _settings.installFiles; }

    // The work (app_work.cpp)
    std::unique_ptr<LoadedGame> loadGame(u64 application);
    SizeEstimate estimateJob(const TitleJob& job, const ConvertOptions& options);
    SizeEstimate measureJob(const TitleJob& job, const ConvertOptions& options);
    bool estimateOnPc(const Row& row, SizeEstimate& estimate, bool measure = false);
    SizeEstimate estimateRow(const Row& row);
    /// The same from reading all of it, writing nothing, for a game a first look can't vouch for.
    SizeEstimate measureRow(const Row& row);
    SizeEstimate workOutRow(const Row& row, bool measure);
    /// First looks, or reading games in full by itself for a thorough analysis.
    void analyze(const std::vector<std::string>& keys, bool measuring = false);
    /// One game's first look, or reading it in full, and what it comes to.
    void workOutOne(const std::string& key, bool measuring, int& tab);
    /// While a game is read in full, games that need a first look get it there and then, the reading waiting where it is. Called by the analyst
    /// between one part of what it reads and the next.
    void firstLooksWhileMeasuring();
    /// copy is a temporary copy beside it while it works, of a solid NSZ, which is said if that's what makes it too much.
    void needRoom(Storage storage, i64 output, i64 copy = 0);
    void needRoomIn(const std::string& folder, i64 output, i64 copy = 0);
    bool goesOnConsole(const Row& row) const;
    /// How much memory a solid NSZ's stream read out of order can be kept in: what's spare, less what small sections are kept in.
    static i64 solidMemory();
    /// What of a row's solid streams may not fit in that memory, and so may go in a temporary copy on the SD card while it's worked on.
    static i64 temporaryCopy(const Row& row);
    bool onTheSdCard(const std::string& path);
    bool replacesInstalled() const { return _settings.replaceInstalled; }
    Storage installTarget() const { return _settings.installToConsole || _consoleOnce ? Storage::Nand : Storage::Sd; }
    /// For what's ticked, and then any of these as well.
    RoomPlan planRoom();
    std::vector<const char*> stepsFor(const Row& row) const;
    std::vector<const char*> stepsOfWorkFor(const Row& row) const;
    /// The games on a list a first look can't vouch for, the ones most likely to be worth it first. Ready only, without the ones being worked on
    /// or that couldn't be read, which a thorough analysis leaves as they are.
    std::vector<std::string> notSureKeysLocked(int tab, bool readyOnly = false);
    /// Whether reading a game in full would say more than its first look did.
    bool unmeasuredLocked(const Row& row);
    /// Why a ticked game is left as it is, as far as is known before anything starts, or nothing if it gets done. The question before
    /// compressing, the room plan and the job all go by it.
    std::string leftAsItIsLocked(const Row& row);
    /// Why an installed game is left as it is whoever ticked it, or nothing: compressed already by this version, or with no game data to compress.
    static std::string nothingToGainReason(const Row& row, const SizeEstimate& estimate);
    /// A list is still coming up, or has games waiting for their first look, which come before reading any game in full.
    bool firstLooksLeftLocked();
    /// What analysis reads in full, in the order it does: the games it could change the answer for first, then the rest.
    std::vector<std::string> measureKeysLocked(int tab);
    void explainMeasuring(int tab);
    void offerToDeleteInstalled();
    std::set<std::string> installedFilesLocked(const std::vector<std::string>& installed, bool say);
    void workPlannedFor(const Row& row, JobKind kind, i64 (&planned)[2]) const;
    void prepareJob(const std::vector<std::string>& keys, JobKind kind);
    std::shared_ptr<Image> iconForJob(const Row& row);
    void loadBigIcon(const Row& row);
    void startGame(const Row& row, const std::vector<const char*>& steps);
    void endGameLocked();
    void noteWorkLocked(const char* label, i64 done);
    double overallLocked(double* secondsLeft);
    float drawExplanation(Canvas& c, float x, float baseline, const std::string& text, float size, float width, float lineHeight, Color color, int maxLines);
    int descriptionLines(const std::vector<OptionItem>& items, float width);
    /// What explains something but didn't fit, on the frame being drawn.
    std::vector<std::string> _cut;
    std::vector<std::string> _fitCut;
    std::vector<std::string> _crowded;
    std::vector<std::string> _headings;
    /// Games that have an icon but got their letters instead, on the frame being drawn, which should never happen.
    std::vector<std::string> _lettersForIcons;
    void noteStepLocked(const char* label);
    void finishJob(const std::string& title, const std::string& detail, const std::string& advice = "");
    InstalledGame whatsInstalled(u64 application, u32 versionComing, bool wholeGame, u64 titleId);
    Replacer replacer();
    bool consoleReadsWhatsMade(const std::vector<std::string>& keys);
    void compress(const std::vector<std::string>& keys);
    std::string batteryTooLow();
    std::string compressSummary(double seconds);
    void compressOne(const Row& row, ResultLine& result, bool& leftOut);
    bool measureBeforeAnything(const Row& row, ResultLine& result);
    void compressInstalled(const Row& row, const SizeEstimate& estimate, const ConvertOptions& options, ResultLine& result);
    void buildExtracted(const Row& row, const SizeEstimate& estimate, const ConvertOptions& options, ResultLine& result);
    void compressFiles(const Row& row, const SizeEstimate& estimate, const ConvertOptions& options, ResultLine& result);
    void saveNsps(const std::vector<const TitleJob*>& jobs, const TitleSet& set, const SizeEstimate& estimate, const ConvertOptions& options, bool leaveCompressed,
                  bool remake, const std::vector<u64>& needFile, ResultLine& result);
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

    // Help, and reporting a problem (app_help.cpp)
    void showHelpTopic(size_t index);
    void saveReport(const std::string& said = "");
    void reportProblem(const std::string& kind = "manual", const std::string& fingerprint = "", const std::string& about = "", bool askWhatHappened = true);
    void sendReport(const std::string& kind, const std::string& fingerprint, const std::string& about, const std::string& said);
    void askAboutSendingProblems();
    std::string deviceName();
    // Problems the app finds itself, in app_problems.cpp.
    std::string problemFolder();
    void problemFound(const std::string& kind, const std::string& message, const std::string& report = "", bool keepNames = false,
        const std::string& knownAs = "");
    void sendWaitingProblems();
    void sendProblemsNow();
    void checkLastTime();
    void watchForFreezes();
    void closingProblems();
    void askAboutStoppedGames();
    void gameDidntStart(const std::string& name, u64 application, bool crashed);
    std::string reportText();
    /// Whether the base game installed is a copy this tool rebuilt, from its program's header. Read once a game, and remembered.
    bool rebuiltHere(const InstalledGame& game, const InstalledMeta& base);
    /// Whether an official update has gone in over a game this tool rebuilt, which can't start like that.
    bool updatedOverRebuilt(const InstalledGame& game);
    /// Whether a copy this rebuilt still says it's only whole with its update, which stops it starting, and sets markedIncomplete if so.
    void judgeMark(InstalledGame& game);
    /// What the overview's line of games that won't start does: says why, and offers to fix them.
    void askToFixMarks();
    /// Takes that off each game's record, and tells the console the version there is the one to start.
    void fixMarks(const std::vector<std::string>& keys);
    /// What that comes to for a game: an update newer than the copy is to be made part of it, and one that isn't blocks it.
    void judgeUpdateOver(InstalledGame& game);
    /// Throws, saying what it takes, if an update to be made part of a game installed as a copy this compressed doesn't fit it.
    void checkUpdateFits(const TitleJob& job, bool installedOverIt);
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
    void onlyThese(const std::vector<std::string>& keys, std::function<void()> action);
    void restoreTicks();
    std::vector<std::string> tickedGames();
    void setTab(int tab);
    /// Where L and R are: the overview is first, then the lists.
    int position() const { return _overview ? 0 : _tab + 1; }
    void goToPosition(int position);
    void showList(int tab, View view);
    /// Left and right on the D-pad: the view beside this one, if there is one.
    void stepView(int direction);
    /// Keeps the game that had the focus, or the nearest one after it, so a change of view never loses the place.
    void setView(View view);
    /// The views left and right go through, which are the same with compression on or off.
    std::vector<View> viewsOffered() const;
    static const char* viewName(View view);
    std::string sortDescription();
    void changeSort(int direction);
    void hideFocused();
    void showHidden();
    void lookElsewhere();
    void chooseFilesFolder();
    void toggleSource(bool& source);
    void toggleWifiFirst();
    /// A view was changed since the settings were last saved, which they are as the app closes.
    bool _viewsChanged = false;
    /// From the overview: both lists looked at again, for what's been plugged in, inserted or shared since.
    void lookAgain();
    /// Measuring every game in full, for exact figures, stops for now, or goes on.
    void stopMeasuring();
    void goOnMeasuring();
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
    /// To the console this time, in place of the SD card, as asked when the card hadn't room.
    void askToCompress(bool toConsole = false);
    void askToStop();
    /// What's selected, or everything that's ticked.
    void askToRemove(bool allTicked);
    MenuFacts menuFacts();
    /// Opens the + menu on a page. Its first page is the one it's opened on, unless it's a page reached from another (root).
    void openMenu(int page = MenuMain, int root = -1);
    void browse(const std::string& title, const std::string& start, const std::string& useDefaultLabel, std::function<void(const std::string&)> choose);
    /// Where Look inside saves what's in games: a folder of its own, apart from where extracted games go, and never looked in for games, so a
    /// few parts of a game saved from inside it can't be taken for the whole of it, or mixed in with an extraction of it.
    std::string filesFromGamesFolder() const;
    /// + -> This game -> Details, or Look inside: the game under the pointer, opened on a thread of its own.
    void openInside(bool details);
    void closeInside();
    void insideReaderFor(const Row& row);
    void readInside(Inside& inside, const Row& row);
    void detailsOf(Inside& inside, const Row& row);
    void previewInside(Inside& inside, int node);
    void saveInside(int node);
    void findInside();
    /// What's shown in the folder on screen: its own, or what a search found.
    const std::vector<int>& insideShownLocked() const;
    bool canLookInside(const Row& row) const;
    /// A to Look inside from the details, and what X does there, as it does on the list: empty when there's nothing for it to do.
    void goInside();
    std::string detailsActionLocked() const;
    /// What - and the + menu do in Look inside: its files shown as bytes or as what they are, saving what's under the pointer, and the top.
    void toggleInsideBytesLocked();
    void saveInsideFocusedLocked();
    void insideToTopLocked();
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
    OptionItem sendProblemsOption(const char* section);
    void compressionChanged();
    void buildOptions();
    void buildWelcome();
    void finishWelcome();
    void openSettings();
    void closeSettings();
    const char* verb() const { return _settings.compress ? "压缩" : "重打包"; }
    /// What X does on the list on the screen: on Game files, when they're installed, it's installing them, compressed as they go in.
    const char* actionWord() const { return actionWordOn(_tab); }
    const char* actionWordOn(int tab) const { return tab == TabFiles && _console && _settings.installFiles ? "安装" : verb(); }
    const char* verbing() const { return _settings.compress ? "正在压缩" : "正在重打包"; }
    const char* verbed() const { return _settings.compress ? "压缩" : "重打包"; }

    // Input (app_input.cpp)
    void showWhatsWaiting();
    void catchUpWithDrives();
    void catchUpWithPcs();
    void updateMain(u32 pressed, const InputState& input, double dt);
    void updateOverview(u32 pressed, const InputState& input, double dt);
    /// A line of the overview: a count of games on one of the lists, which A opens that list on. Looking and Nothing are what a list
    /// with nothing to count says instead, filling its card.
    struct OverviewLine {
        /// NotSure is the games a first look can't vouch for, which A on it measures. WontStart is installed copies an older version left
        /// saying they need their update, which A on it offers to fix. Developer is the games their developer compressed that are worth
        /// compressing again, but aren't ticked by themselves, as their updates then need their original: A on it opens the list.
        enum Kind { Looking, ToDo, ToInstall, Done, Nothing, NotSure, WontStart, Developer };
        int tab = 0;
        Kind kind = Nothing;
        std::string text;
        /// The line under it: what the games come to, or why there's nothing to count.
        std::string sub;
        /// What the ticked games save, and that as a share of what they are now. Below zero when there's no telling yet.
        i64 saved = 0;
        double share = -1;
        /// Whether that's what they're sure to save, all an estimate can promise, so it says "at least".
        bool atLeast = false;
        /// How many ticked games its button does, and how many of those are measured first, which the text and figures leave out.
        int count = 0, notSure = 0;
        /// Whether X does anything on it: there's something ticked to do, or files to install.
        bool acts = false;
        /// No game files were found at all, so the card says where they're looked for, with a button to change that.
        bool sources = false;
        /// The compressed files a ToInstall line installs.
        std::vector<std::string> keys;
        /// What a card opens with, at its top: what's to do, or why there's nothing to count. The others go at its bottom.
        static bool atTop(Kind kind) { return kind == ToDo || kind == Looking || kind == Nothing; }
    };
    /// One of the games a ToDo line counts, among those saving the most.
    struct OverviewGame {
        std::string key, name;
        std::shared_ptr<Image> icon;
        Color tile;
        i64 saved = 0;
    };
    /// A list as the overview has it: how many games, and the lines saying what there is to do with them.
    struct OverviewCard {
        int games = 0;
        i64 bytes = 0;
        std::vector<OverviewLine> lines;
        std::vector<OverviewGame> biggest;
        int more = 0;
    };
    /// Free space where games go, now and once what's ticked is compressed. Only installed games that get replaced free any.
    struct OverviewMeter {
        const char* label = "";
        i64 free = -1, total = 0, frees = 0;
    };
    struct Overview {
        std::string headline;
        std::vector<OverviewMeter> meters;
        OverviewCard cards[2];
    };
    /// Somewhere the pointer stops on the overview. A line, which A opens the list on; one of the games under a ToDo line, which A opens the
    /// list on with the pointer on that game; the card's button that does what it's there for, which X does from anywhere on the card too;
    /// the one beside it that opens the list to choose from; or, with no game files found, the one that says where to look for them. The
    /// buttons say what they do, so nothing has to be looked for at the bottom of the screen.
    struct OverviewStop {
        enum Kind { Line, Game, Act, Choose, Sources };
        Kind kind = Line;
        int tab = 0;
        /// The line, or for a game the line it's under, as its place among its card's lines.
        int line = 0;
        /// The game's place among its card's biggest, and its key, or -1 and nothing for the line itself.
        int game = -1;
        std::string key;
        Rect rect;
    };
    /// Where each card goes, and every place on it the pointer stops, in the order they're drawn, the first card's first. Worked out the
    /// same way for drawing the overview as for moving over it.
    struct OverviewLayout {
        Rect cards[2], bodies[2];
        /// What's ticked on each card, and what that saves, above its buttons.
        Rect summaries[2];
        /// Where the lines at the bottom of each card start.
        int secondTop[2] = {0, 0};
        std::vector<OverviewStop> stops;
    };
    /// What the pointer's on: a line by its kind, as a card has one of each, or a game by its key. Lines and games come and go while the
    /// overview is being worked out, so a place in the order wouldn't stay on the same thing.
    struct OverviewPointer {
        int tab = 0;
        OverviewLine::Kind kind = OverviewLine::ToDo;
        /// Where on the card: the button that does what it's there for, to start with.
        OverviewStop::Kind stop = OverviewStop::Act;
        std::string game;
    };
    /// Worked out again only when something it goes by has changed, as it means going through every game.
    const Overview& overviewLocked();
    Overview workOutOverviewLocked();
    u64 overviewKeyLocked() const;
    void openOverviewLine(const OverviewLine& line);
    void actOnOverviewLine(const OverviewLine& line);
    void openOverviewGame(int tab, const std::string& key);
    /// A, or a second tap, on whatever the pointer's on.
    void pressOverviewStop(const OverviewStop& stop, const OverviewLine& line);
    void pointOverviewAt(const OverviewStop& stop, const OverviewLine& line);
    /// The line the card's button does what it's there for on, or -1: what's ticked to do, or else the compressed files there are to install.
    static int primaryLine(const OverviewCard& card);
    /// What that button says: what it does, and to how many games.
    std::string actLabel(const OverviewLine& line) const;
    /// Where the pointer goes from a stop, with up or down, or left or right.
    static int nextOverviewStop(const OverviewLayout& layout, int from, int move, int across);
    OverviewLayout overviewLayout(const Overview& overview) const;
    /// Which stop the pointer's on: the same game, or else its line, or else the first on its card.
    int overviewStopIndex(const Overview& overview, const OverviewLayout& layout) const;
    void updateOptionItem(OptionItem& item, u32 pressed, const InputState& input, double dt);
    void updateOptions(u32 pressed, const InputState& input, double dt);
    void updateSettings(u32 pressed, const InputState& input, double dt);
    void updateWelcome(u32 pressed, const InputState& input, double dt);
    void updateResults(u32 pressed, const InputState& input, double dt);
    void updateBrowse(u32 pressed, const InputState& input, double dt);
    void updateDetails(u32 pressed, const InputState& input, double dt);
    void updateInside(u32 pressed, const InputState& input, double dt);
    /// The buttons with the lock held, and what's left to do once it's let go: closing waits for the reader, and finding for the typing.
    enum class InsideAfter { Nothing, Close, Find, Menu };
    InsideAfter updateInsideLocked(u32 pressed, const InputState& input, double dt);
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
    void drawOverview(Canvas& c);
    void drawOverviewFooter(Canvas& c);
    void drawTopBar(Canvas& c);
    void drawStorage(Canvas& c, int x, int width, const char* label, i64 free, i64 total, i64 frees);
    /// A bar of how full a place is: grey for what's used, the accent colour for what compressing what's ticked frees.
    void drawMeterBar(Canvas& c, Rect bar, i64 free, i64 total, i64 frees);
    /// Ticked games on the console that would get smaller, by where they are. Only replacing them frees anything.
    i64 freedOnLocked(Storage storage) const;
    std::string whereGameFilesAre() const;
    void drawOverviewCard(Canvas& c, int tab, const OverviewCard& card, const OverviewLayout& layout, int focus);
    void drawOverviewButton(Canvas& c, Rect r, const std::string& label, bool primary, bool lead, bool focused);
    /// What a card with nothing to count says instead, in the middle of what's left of it.
    void drawOverviewReason(Canvas& c, Rect r, const OverviewLine& line);
    std::shared_ptr<Image> smallIcon(const std::shared_ptr<Image>& icon, int size);
    void drawColumns(Canvas& c, const std::vector<Row*>& rows);
    /// What a row says, which is what's drawn and what the tests compare.
    struct ShownRow {
        enum Kind { Plain, Faint, Bad, Good, Warn };
        std::string second;
        Kind secondKind = Plain;
        std::string now, after, saved, share, status;
        /// Sure to save nothing: After is its size now, and Saves says so in words.
        bool nothing = false;
        /// Only a first look stands behind it, which can't say what it saves: Saves says it isn't sure.
        bool notSure = false;
        /// On Done, Saved is what compressing it freed, under a heading of Freed.
        bool freed = false;
        /// Which of two files of the same game it is, said after the second line.
        std::string whichFile;
    };
    ShownRow shownLocked(const Row& row) const;
    /// The row as the view it's on shows it, which on Done is what's become of the game rather than what compressing it would do.
    ShownRow shownInViewLocked(const Row& row) const;
    void drawRow(Canvas& c, const Row& row, int y);
    void drawEmpty(Canvas& c, const std::string& title, const std::string& detail, bool spinner);
    void drawEmptyList(Canvas& c);
    void drawFooter(Canvas& c, const std::vector<Row*>& rows);
    void drawHints(Canvas& c, const std::vector<Hint>& hints);
    void drawButtonGlyph(Canvas& c, float cx, float cy, const char* name, float radius, Color fill, Color ink);
    void drawOptions(Canvas& c);
    void drawSettings(Canvas& c);
    void drawWelcome(Canvas& c);
    void drawChangeArrows(Canvas& c, float left, float right, float mid, Color ink);
    void drawJob(Canvas& c);
    void drawResults(Canvas& c);
    void drawNoKeys(Canvas& c);
    void drawBrowse(Canvas& c);
    void drawDetails(Canvas& c);
    void drawInside(Canvas& c);
    void drawInsideFull(Canvas& c);
    void drawDialog(Canvas& c);
    void drawMenu(Canvas& c);
    void drawMenuBehind(Canvas& c);
    bool drawOverMenuBehind(Canvas& c);
    bool drawOverDialogBehind(Canvas& c);
    /// A question over what's been drawn so far, which is darkened and kept first while it rises, unless something in it is still moving.
    void drawDialogOver(Canvas& c, bool behindMoving);
    bool dialogRising() const;
    void drawToast(Canvas& c);
    void drawTick(Canvas& c, float cx, float cy, float radius, bool ticked, bool enabled);
    void drawTile(Canvas& c, const std::shared_ptr<Image>& icon, Color tile, const std::string& name, int x, int y, int size, int radius);
    void drawSpinner(Canvas& c, float cx, float cy, float radius);
    void drawBar(Canvas& c, Rect r, double fraction, Color fill);
    /// What compressing a game saves, as a share of what it is now, from the left on a quiet track, in one green that's stronger the more it
    /// saves (shareColor, given what it comes to). Faint for a game that isn't ticked.
    void drawShareBar(Canvas& c, Rect r, double share, bool faint = false);
    Color shareColor(double share) const;
    /// Details or Look inside still reading something, which keeps a spinner turning.
    bool insideLoadingLocked() const;

    Platform& _platform;
    ConsoleApi* _console = nullptr;
    Font _font;
    /// Bytes and text inside games: Inconsolata, with the system's fonts behind it for the characters it hasn't got.
    Font _mono;
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
    /// What the version before worked out with compression on, by this version's keys, until its game comes up.
    std::map<std::string, std::string> _previousAnswers;
    bool _cacheChanged = false;
    double _cacheSavedAt = 0;
    /// What both copies of analysis.txt are known to hold, end and all, so that what would come out the same isn't written again. Empty when
    /// that isn't known.
    std::string _cacheOnCard;
    std::set<std::string> _hidden;
    std::map<std::string, IndexedFile> _fileIndex;
    bool _fileIndexChanged = false;
    /// What the list of game files was last made from, which it's made from again when a setting changes which copy of a game is used.
    std::vector<std::string> _listedPaths;
    FoundGames _found;
    bool _listChanged = false;
    sys::Mutex _listSaving;
    sys::Mutex _indexSaving;
    bool _sortedByGain[TabCount] = {false, false};
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
    /// It's reading games in full, for a thorough analysis, rather than taking first looks.
    std::atomic<bool> _analystMeasuring{false};
    /// It was started on reading games in full, which it still is while it takes first looks in the middle of it, when it isn't measuring:
    /// turning Thorough off stops it by this. Set only where it's started, and cleared once it's ended.
    std::atomic<bool> _analystStartedMeasuring{false};
    /// The analyst is taking first looks in the middle of reading a game in full, which waits for them, and when it last looked for any. It's
    /// set and cleared under the lock, as the footer goes by it.
    bool _lookingWhileMeasuring = false;
    double _firstLooksCheckedAt = 0;
    /// The games this run of analysis goes through, in order, and which it's on. Games given their first look in the middle of measuring join
    /// it right after the one being measured, so they're measured next, and the count says so. Only the analyst's own.
    std::vector<std::string> _analysisQueue;
    size_t _analysisAt = 0;
    /// While first looks are taken in the middle of measuring, the footer goes on showing the game being measured, as it was, rather than
    /// flicking to a look that takes a second: the game in its row says it's being looked at.
    struct MeasuringShown {
        std::string title;
        int index = 0, count = 0, tab = -1;
        double fraction = 0;
    } _measuringShown;
    /// What the footer says about analysis (MeasuringShown).
    struct AnalysisShown {
        bool measuring = false;
        std::string title;
        int index = 0, count = 0, tab = -1;
        double fraction = 0;
    };
    AnalysisShown analysisShownLocked() const {
        if (_lookingWhileMeasuring)
            return AnalysisShown{true, _measuringShown.title, _measuringShown.index, _measuringShown.count, _measuringShown.tab, _measuringShown.fraction};
        return AnalysisShown{_analystMeasuring, _analysisTitle, _analysisIndex, _analysisCount, _analyzingTab, analysisFractionLocked()};
    }
    /// Games from files go on the console for this job, as the SD card hadn't room for them and the console had.
    std::atomic<bool> _consoleOnce{false};
    /// What the last job installed from files that were kept, for asking the once whether to delete them.
    std::vector<std::string> _installedKept;
    /// Measuring by itself waits while the battery's low, which is only asked about now and then.
    std::atomic<double> _measureAgainAt{0};
    int _analysisIndex = 0;
    int _analysisCount = 0;
    std::string _analysisTitle;
    /// Measuring, how far it's got by the game data it reads, as games differ in size by thousands of times: what all of this run's games come
    /// to, what the ones finished came to, and the one being read, image by image, as the engine says how far it's got.
    i64 _analysisBytesTotal = 0, _analysisBytesBefore = 0, _analysisRowBytes = 0;
    i64 _analysisImagesDone = 0, _analysisImageDone = 0, _analysisImageTotal = 0;
    /// How fast this console has measured since the app opened, sampled and exactly, by the game data read and the time it took, which the
    /// time it says is left goes by.
    double _measuredBytes[2] = {}, _measuredSeconds[2] = {};
    /// How much of the game data the one being measured has been read, from what the engine said.
    i64 measuringDoneLocked() const { return std::min(_analysisImagesDone + _analysisImageDone, _analysisRowBytes); }
    /// How far analysis has got: measuring, by the game data read, and a first look, by the games, which each take about as long.
    double analysisFractionLocked() const {
        if (_analystMeasuring && _analysisBytesTotal > 0)
            return std::min(1.0, (double)(_analysisBytesBefore + measuringDoneLocked()) / (double)_analysisBytesTotal);
        return _analysisCount > 0 ? (double)std::max(_analysisIndex - 1, 0) / _analysisCount : 0;
    }
    /// What reading a game's data takes, in bytes a second: what this console has done so far, starting from what it's been seen to do.
    double measureRateLocked(bool exact) const;
    /// About how long measuring what's left takes, on one list or on all of them.
    std::string measureTimeLocked(int tab);
    bool _autoAnalyze[TabCount] = {true, true};
    /// The list whose analysis gave way to what X asked for, to go on by itself once that's been answered or done. -1 if none did.
    int _analysisPausedTab = -1;
    /// A list coming up gives way to the one on the screen, without taking analyzing with it.
    std::atomic<bool> _scanStop{false};
    /// B was pressed while the list on the screen was being looked through: what's been found stays, and it isn't looked through again until it's refreshed.
    std::atomic<bool> _scanStoppedByHand{false};
    /// For the screen to say so, as it can only be said from there.
    std::atomic<bool> _sayStoppedLooking{false};
    std::atomic<bool> _scanned[TabCount] = {{false}, {false}};
    bool _scanWanted[TabCount] = {false, false};
    std::atomic<int> _scanningTab{-1};
    std::atomic<bool> _rescanInstalled{false};
    /// Whether what the last time may have left half replaced has been dealt with. Until it has, nothing is replaced (App::replacer).
    std::atomic<bool> _journalChecked{false};
    /// What couldn't be read as the app opened because the system turned it down, which is read again once it's open (App::startingUp).
    std::atomic<bool> _listRefused{false};
    std::atomic<bool> _indexRefused{false};
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
    /// What the list says under the game's name, such as its version and where it is.
    std::string _jobDetail;
    /// Every game this time, in the order they're done, which the progress screen lists.
    std::vector<std::string> _jobKeys;
    /// Free space on the SD card, the console and a USB drive when this started, to say afterwards how much more there is.
    i64 _freeBefore[3] = {-1, -1, -1};
    /// How many results fit on the screen, which scrolling goes by.
    int _resultsShown = 7;
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
    /// What to do about what stopped a job before it got to any game, said under "未做任何改动".
    std::string _resultsAdvice;
    i64 _freeSd = -1, _totalSd = 0, _freeNand = -1, _totalNand = 0, _freeUsb = -1, _totalUsb = 0;
    /// What went wrong that nothing was expecting, which gets shown, as there may be no log to look at.
    std::string _unexpected;

    // Updating
    sys::Thread _updateThread;
    /// Sending a report, beside the screen, which shows what came of it once it's back.
    sys::Thread _reportThread;
    std::atomic<bool> _reporting{false};
    /// Problems the app found itself going to the developer, beside the screen, and the watch on the screen being drawn.
    sys::Thread _problemThread;
    sys::Thread _watchThread;
    sys::Mutex _problemMutex;
    std::atomic<bool> _problemsSending{false};
    std::atomic<bool> _problemsStop{false};
    std::atomic<bool> _watchStop{false};
    /// The system's keyboard is open, which holds up the screen without the app having stopped answering.
    std::atomic<bool> _watchPaused{false};
    std::atomic<double> _aliveAt{0};
    /// When the app was opened before this time, for what's happened since, or 0 the first time.
    long long _lastOpened = 0;
    bool _askedAboutGames = false;
    /// The versions the console wants of games have gone in the log, which is once each time the app opens.
    bool _versionsLogged = false;
    /// What Atmosphere says crashed since the app was last open, and what the console's play history has closing within moments of starting,
    /// by program, read the once, and when to look at it again.
    bool _stoppedListed = false;
    struct StoppedProgram {
        u64 program = 0;
        /// Atmosphere's report of it, or empty when it's from the play history, which says how many seconds it ran.
        std::string crash;
        long long ranFor = 0;
    };
    std::vector<StoppedProgram> _stoppedPrograms;
    double _nextStoppedCheck = 0;
    std::atomic<bool> _updating{false};
    std::atomic<bool> _updateStop{false};
    std::string _updateState;
    std::string _updateNoticeTitle;
    std::string _updateNoticeText;
    std::string _updatedProgram;

    // The log
    sys::Mutex _logMutex;
    std::unique_ptr<sys::File> _log;
    /// The log being kept, while it's kept, even when it's closed because the card turned down opening it again: it's asked again a second
    /// later. Empty with no log.
    std::string _logPath;
    i64 _logPosition = 0;
    double _logClosedAt = 0;
    /// How many more times the log, kept as log-other.txt as the system turned log.txt down, is to be given that name, since when, and when
    /// next.
    int _logNameTries = 0;
    double _logNameTurnedDownAt = 0, _logNameNextTry = 0;
    /// When the log, turned down, is to be asked for again.
    double _logRetryAt = 0;
    /// What's been said since the card last let the log be written, which goes in once it does.
    std::string _logUnwritten;
    i64 _refusalsLogged = 0;
    /// What's said before there's any knowing whether there's to be a log, which is in the settings.
    std::vector<std::string> _earlyLog;
    /// The last of what's been said, kept whether or not there's a log, for a report.
    std::deque<std::string> _recentLog;
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
    View _view[TabCount] = {View::ToDo, View::ToDo};
    /// The game that had the focus when the view last changed, and where it was on the screen, kept through a view with nothing in it, or one
    /// without it that's only gone through, the pointer left at its top.
    std::string _viewAnchor[TabCount];
    double _anchorOnScreen[TabCount] = {0, 0};
    bool _anchorKept[TabCount] = {false, false};
    /// The app opens on the overview, which says what there is to do on each list. _tab is then the list it last showed.
    bool _overview = true;
    /// Which of the overview's lines has the focus, counting the first card's first.
    OverviewPointer _overviewAt;
    Overview _overviewCache;
    u64 _overviewCacheKey = 0;
    bool _overviewCached = false;
    /// Icons drawn small, made once for each size each is drawn at, and made again if the icon they're from has gone. The size is part of
    /// what they're found by: the overview draws a game at one size and the results at another, and one made for the first used to be given
    /// to the second, which then drew the game's letters instead.
    std::map<std::pair<const Image*, int>, std::pair<std::weak_ptr<Image>, std::shared_ptr<Image>>> _smallIcons;
    int _focus[TabCount] = {0, 0};
    int _focusSeen[TabCount] = {0, 0};
    double _scroll[TabCount] = {0, 0};
    int _tickStep[TabCount] = {0, 0};
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
    /// What's behind a question while it rises, likewise, for the one with this serial.
    std::vector<u32> _dialogBehind;
    u64 _dialogBehindFor = 0;
    int _glideList = -1;
    double _glideShown = 0;
    double _glideStep = 1;
    std::vector<OptionItem> _options;
    int _optionFocus = 0;
    /// The welcome was had again from the settings, so finishing it goes back there.
    bool _welcomeFromSettings = false;
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
    std::unique_ptr<Inside> _inside;
    /// Reads what Details and Look inside show, one request at a time, the newest one first: the game, then what's under the pointer, or a
    /// file or folder to save.
    std::unique_ptr<sys::Thread> _insideThread;
    sys::CondVar _insideWake;
    std::atomic<bool> _insideStop{false};
    /// What the reader is to show next, and what it's reading for that now, which it gives up on over a network when it's no longer wanted.
    std::atomic<int> _insideWant{-1};
    std::atomic<int> _insidePreviewing{-1};
    int _insideSave = -1;
    std::atomic<bool> _insideSaveStop{false};
    Screen _browseFrom = Screen::Main;
    std::vector<Hit> _hits;
    std::string _toast;
    double _toastUntil = 0;
    double _time = 0;
    /// How long the last frame took to draw, for the tests to find the slow ones.
    double _lastDrawMs = 0;
    // When what's on the screen last changed, for the little movement that shows it: a screen fading in, the + menu and a question rising
    // into place, a message coming and going, and the bar on Home's cards.
    Screen _screenSeen = Screen::Main;
    double _screenSince = -1, _menuSince = -1, _dialogSince = -1, _toastSince = -1;
    const void* _menuSeen = nullptr;
    const void* _dialogSeen = nullptr;
    std::string _toastSeen;
    double _cardShareShown[2] = {-1, -1};
    /// Where the filled-in view is on its way to, on each list.
    double _viewShown[2] = {-1, -1};
    double _lastWorkDraw = 0;
    std::atomic<bool> _dirty{true};
    bool _quit = false;

    // The buttons and the touch screen
    u32 _lastHeld = 0;
    /// When a button was last pressed or the screen touched, so the list only takes a new order while nobody's moving through it.
    double _lastPressAt = -10;
    /// Analysis has found something that puts a list in a new order.
    std::atomic<bool> _resortWanted{false};
    double _repeatTimer = 0;
    u32 _repeatButton = 0;
    bool _touchWasDown = false;
    int _touchStartX = 0, _touchStartY = 0, _touchLastY = 0;
    bool _touchDragged = false;
};

}  // namespace hbp
