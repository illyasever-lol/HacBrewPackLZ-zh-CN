// Checks the app against itself rather than against what someone thought to write down. A pretend person changes settings, ticks games,
// sorts and runs jobs, in an order that comes from a seed, and after each step:
//   - a fresh start of the app, with the same files, console and settings, has to show the same lists, as anything else is something
//     the running app didn't keep up with;
//   - now and then a start from nothing, with nothing remembered, has to come to the same sizes, within what they're given as;
//   - what a row says it comes to is checked against what converting it really comes to;
//   - what a question says about games, such as their coming out the same size, is checked against what their rows say.
//
//   app_test -k keys folder shots check [seed] [steps] base.nsp update.nsp dlc.nsp other.nsp [more game files...]
//
// other.nsp, a game of its own, goes on the pretend console as it is, and on the SD card as a compressed copy. Any more go on the SD card
// beside the first three, such as a game whose update holds part of it more than once, and one compressed by Nintendo with its update.
#include "../app/app_internal.h"
#include "app_test_support.h"
#include "sim_console.h"

#include <algorithm>
#include <map>
#include <random>
#include <set>

namespace apptest {

using hbp::app::fileNameOf;
using hbp::app::joined;

namespace {

struct Settings {
    bool compress = true;
    bool installFiles = false;
    bool hideDone = false;
    int minSaving = 0;
    int sort = 0;
    /// As the app has it to begin with.
    bool thorough = true;
    /// The question asked once has been, which is how every check but the one about it starts.
    bool deleteAsked = true;
};

std::string describe(const Settings& s) {
    static const char* const sorts[] = {"best saving", "size", "name"};
    return format("compression %s, game files %s, done %s, minimum %d%%, by %s, analysis %s", s.compress ? "on" : "off", s.installFiles ? "installed" : "saved",
                  s.hideDone ? "hidden" : "shown", s.minSaving, sorts[s.sort], s.thorough ? "thorough" : "quick");
}

void copyFile(const std::string& from, const std::string& to) {
    Bytes bytes = readWhole(from);
    sys::File out(to, sys::File::Mode::Create);
    if (!bytes.empty())
        out.write(0, bytes.data(), bytes.size());
}

/// What's remembered, without what's being worked on or the log.
void copyRemembered(const std::string& from, const std::string& to) {
    sys::makeDirectories(to);
    if (!sys::exists(from))
        return;
    for (auto& entry : sys::list(from)) {
        if (entry.name == "temp" || entry.name == "log.txt" || entry.name == "log.old.txt")
            continue;
        if (entry.isDirectory)
            copyRemembered(from + "/" + entry.name, to + "/" + entry.name);
        else
            copyFile(from + "/" + entry.name, to + "/" + entry.name);
    }
}

/// The same SD card and console, with what's remembered somewhere else, so a second app can start while the first one is still going.
class SharedPlatform : public HostPlatform {
public:
    SharedPlatform(const std::string& folder, const std::string& keys, ConsoleApi* console, std::string remembered)
        : HostPlatform(folder, keys, console), _remembered(std::move(remembered)) {}
    std::string dataFolder() override { return _remembered + "/data"; }
    std::string backupFolder() override { return _remembered + "/config"; }

private:
    std::string _remembered;
};

class Checker {
public:
    Checker(std::string keys, std::string folder, std::string shots, std::vector<std::string> nsps)
        : _keys(std::move(keys)), _folder(std::move(folder)), _shots(std::move(shots)), _nsps(std::move(nsps)) {
        _keyset.load(_keys);
        _root = _folder + "/check";
    }

    int run(unsigned seed, int steps);

private:
    void setUp(const Settings& settings);
    void writeSettings(const Settings& settings);
    App::Snapshot freshStart(const App::Snapshot& running, bool fromNothing, const std::string& shot);
    void settleBothTabs(Person& person, App& app, int tab);
    void checkOverview(Person& person, App& app, const std::string& step);
    void checkInstallFromOverview();
    void checkMeasure();
    void checkCopies();
    void checkThorough();
    void checkVeryOldCopy();
    void checkOffers();
    void checkEveryEstimate();
    void compare(const App::Snapshot& running, const App::Snapshot& fresh, const std::string& step, bool fromNothing);
    void checkSizes(const App::Snapshot& running, const std::string& step);
    void checkQuestion(const App::Snapshot& asked, const std::string& step);
    std::string step(Person& person, App& app, std::mt19937& random);
    void backToList(Person& person, App& app);
    Settings settingsOf(App& app);

    std::string _keys, _folder, _shots, _root;
    std::vector<std::string> _nsps;
    Keyset _keyset;
    std::unique_ptr<SimConsole> _console;
    std::string _compressedCopy;
    /// What converting each game really comes to, by its files and whether it's compressed.
    std::map<std::string, i64> _realSizes;
    /// What of it was found held again while compressing, which no estimate knows of.
    std::map<std::string, i64> _foundSizes;
    int _jobsRun = 0;
};

void Checker::writeSettings(const Settings& s) {
    sys::removeFile(_root + "/config/settings.txt");
    sys::makeDirectories(_root + "/data");
    writeText(_root + "/data/settings.txt", format("welcomed=1\nproblemsasked=1\nwritelog=1\nmoresettings=1\ncheckwritten=0\nfrompcnetwork=0\nfrompccable=0\nupdates=0\n"
                                                   "compress=%d\ninstallgamefiles=%d\nhidecompressed=%d\nminimumsaving=%d\nsort=%d\nmeasureall=%d\ndeleteasked=%d\n",
                                                   s.compress ? 1 : 0, s.installFiles ? 1 : 0, s.hideDone ? 1 : 0, s.minSaving, s.sort, s.thorough ? 1 : 0,
                                                   s.deleteAsked ? 1 : 0));
}

void Checker::setUp(const Settings& settings) {
    _console.reset();
    sys::removeTree(_root);
    sys::makeDirectories(_root + "/sd/games");
    for (size_t i = 0; i < _nsps.size(); i++) {
        if (i != 3)
            copyFile(_nsps[i], _root + "/sd/games/" + fileNameOf(_nsps[i]));
    }

    // The compressed copy is made once, as making it takes a while.
    if (_nsps.size() > 3) {
        std::string made = _folder + "/check_made";
        if (_compressedCopy.empty()) {
            sys::removeTree(made);
            sys::makeDirectories(made);
            TitleSet set(_keyset);
            set.add(std::make_shared<FileArchive>(_nsps[3]));
            set.group();
            ConvertOptions options;
            options.tempDir = made;
            options.threads = sys::workerCount();
            for (auto& job : set.jobs) {
                _compressedCopy = made + "/" + outputFileName(job, set, options);
                NspOutput output(_compressedCopy, (int)job.contents.size(), 0, true);
                TitleConverter(options, _keyset).run(job, output);
            }
        }
        copyFile(_compressedCopy, _root + "/sd/games/" + fileNameOf(_compressedCopy));
    }

    _console = std::make_unique<SimConsole>(_root + "/console", _keyset);
    if (_nsps.size() > 3)
        _console->install({_nsps[3]}, Storage::Sd);
    for (auto& path : _nsps) {
        TitleSet set(_keyset);
        set.add(std::make_shared<FileArchive>(path));
        set.group();
        for (auto& job : set.jobs) {
            if (job.isBaseGame() && !job.titleName.empty())
                _console->setAppearance(job.main->meta.titleId, job.titleName, readTitleIcon(job));
        }
    }
    writeSettings(settings);
}

/// Each list is gone to, so both get read and analyzed, and then the one asked for.
void Checker::settleBothTabs(Person& person, App& app, int tab) {
    person.waitUntilIdle();
    for (int t : {1 - tab, tab}) {
        goToTab(person, app, t);
        person.waitUntilIdle();
    }
    person.settle();
}

/// Started again, the lists show what's to do. The running app's lists may show something else, so the fresh one is changed to match.
App::Snapshot Checker::freshStart(const App::Snapshot& running, bool fromNothing, const std::string& shot) {
    int tab = running.tab < 0 ? 0 : running.tab;
    std::string remembered = _root + "/fresh";
    sys::removeTree(remembered);
    copyRemembered(_root + "/data", remembered + "/data");
    copyRemembered(_root + "/config", remembered + "/config");
    if (fromNothing) {
        for (const char* where : {"/data/", "/config/"}) {
            for (const char* name : {"analysis.txt", "list.cache", "files.cache"})
                sys::removeFile(remembered + where + name);
        }
    }
    SharedPlatform platform(_root, _keys, _console.get(), remembered);
    App app(platform);
    app.start();
    Person person(app, _shots);
    settleBothTabs(person, app, tab);
    // Left and right go through To do, Done and Everything, in that order, and stop at either end.
    auto place = [](const std::string& view) { return view == "To do" ? 0 : view == "Done" ? 1 : 2; };
    for (int t : {1 - tab, tab}) {
        goToTab(person, app, t);
        for (int i = 0; i < 4 && app.snapshot().views[t] != running.views[t]; i++)
            person.press(place(app.snapshot().views[t]) < place(running.views[t]) ? ButtonRight : ButtonLeft);
    }
    if (running.tab < 0)
        goToTab(person, app, -1);
    person.settle();
    person.shot(shot);
    return app.snapshot();
}

/// The overview counts what the lists show when they show what's to do: what's ticked and what that saves, and what's done.
void Checker::checkOverview(Person& person, App& app, const std::string& step) {
    int tab = app.snapshot().tab;
    goToTab(person, app, -1);
    person.settle();
    App::Snapshot s = app.snapshot();
    static const char* const tabs[] = {"Installed games", "Game files"};
    for (int t = 0; t < 2; t++) {
        if (!s.scanned[t])
            continue;
        int ticked = 0, done = 0, saving = 0, notSure = 0, unmeasured = 0;
        i64 saved = 0;
        for (auto& row : s.all) {
            if (row.tab != t)
                continue;
            done += row.inDone ? 1 : 0;
            // A copy an older version made waits under Done, and counts for Measure all the same.
            notSure += (row.inToDo || (row.olderCopy && row.inDone)) && row.notSure && !row.blocked && row.hasEstimate ? 1 : 0;
            unmeasured += row.unmeasured ? 1 : 0;
            if (row.inToDo && row.ticked && !row.blocked) {
                ticked++;
                saved += row.savedBytes;
                saving += row.hasEstimate && row.savedBytes > 0 ? 1 : 0;
            }
        }
        std::string toDo, toDoLine, doneLine;
        i64 lineSaved = 0;
        int shown = 0, lineCount = 0, lineNotSure = 0;
        for (auto& line : s.overviewLines) {
            if (line.tab == t && line.kind == "ToDo") {
                toDo = line.text + " (" + line.sub + ")";
                toDoLine = line.text;
                lineSaved = line.saved;
                shown = line.shown;
                lineCount = line.count;
            }
            if (line.tab == t && line.kind == "NotSure")
                lineNotSure = line.count;
            if (line.tab == t && line.kind == "Done")
                doneLine = line.text;
            // What can still be installed has a line of its own, and isn't counted again as done.
            if (line.tab == t && line.kind == "ToInstall")
                done -= atoi(line.text.c_str());
        }
        std::string where = format("%s: the overview's %s", step.c_str(), tabs[t]);
        // "3 to compress" counts what's ticked to do, and "1 game, none ticked" none, and its button does the same. What a first look can't
        // vouch for has a line of its own, ticked or not, with compression on. What's under the first can say "to" as well.
        int countShown = toDoLine.find(" to ") != std::string::npos ? atoi(toDoLine.c_str()) : 0;
        int waiting = s.thorough ? unmeasured : notSure;
        bool compressing = std::any_of(s.overviewLines.begin(), s.overviewLines.end(), [](const App::Snapshot::Line& l) { return l.kind == "NotSure"; }) || waiting == 0;
        report(where + " counts what's ticked to do", countShown == ticked && (lineNotSure == waiting || !compressing) && (ticked == 0 || lineCount == ticked),
               format("it says \"%s\", %d on its line about measuring, its button %d, and %d are ticked, %d to measure", toDo.c_str(), lineNotSure, lineCount, ticked,
                      waiting));
        if (ticked > 0)
            report(where + " says what that saves", lineSaved == saved, format("it says %s, and they save %s", formatSize(lineSaved).c_str(), formatSize(saved).c_str()));
        if (ticked > 0)
            report(where + " shows the games saving the most", shown == std::min(saving, 3), format("it shows %d, and %d save something", shown, saving));
        report(where + " counts what's done", (done == 0 && doneLine.empty()) || atoi(doneLine.c_str()) == done, format("it says \"%s\", and %d are done", doneLine.c_str(), done));
    }
    goToTab(person, app, tab);
}

std::string withoutDigits(const std::string& text) {
    std::string out;
    for (char c : text)
        out += isdigit((unsigned char)c) ? '#' : c;
    return out;
}

void Checker::compare(const App::Snapshot& running, const App::Snapshot& fresh, const std::string& step, bool fromNothing) {
    static const char* const tabs[] = {"Installed games", "Game files"};
    std::vector<std::string> differences;
    auto differ = [&](const std::string& what) { differences.push_back(what); };

    for (int tab = 0; tab < 2; tab++) {
        // A list that's never been gone to hasn't been read yet.
        if (!running.scanned[tab])
            continue;
        std::vector<const App::FullRow*> a, b;
        int hiddenA = 0, hiddenB = 0;
        for (auto& row : running.all) {
            if (row.tab == tab)
                (row.visible ? a.push_back(&row) : (void)hiddenA++);
        }
        for (auto& row : fresh.all) {
            if (row.tab == tab)
                (row.visible ? b.push_back(&row) : (void)hiddenB++);
        }
        std::string where = tabs[tab];
        std::map<std::string, const App::FullRow*> byKey;
        for (auto* row : b)
            byKey[row->key] = row;
        // What's been finished since the app opened is under Done, which a fresh start doesn't know about: it finds the original as it was.
        std::set<std::string> doneHere;
        int doneHereHidden = 0;
        for (auto& row : running.all) {
            if (row.tab != tab || row.state != "Done")
                continue;
            doneHere.insert(row.key);
            doneHereHidden += !row.visible && byKey.count(row.key) ? 1 : 0;
        }
        std::set<std::string> inA;
        for (auto* row : a) {
            inA.insert(row->key);
            if (!byKey.count(row->key))
                differ(format("%s: '%s' is listed, but not after a fresh start (%s)", where.c_str(), row->name.c_str(), row->second.c_str()));
        }
        for (auto* row : b) {
            if (!inA.count(row->key) && !doneHere.count(row->key))
                differ(format("%s: '%s' isn't listed, but is after a fresh start (%s)", where.c_str(), row->name.c_str(), row->second.c_str()));
        }
        if (hiddenA - doneHereHidden != hiddenB) {
            auto hiddenIn = [&](const App::Snapshot& s) {
                std::vector<std::string> names;
                for (auto& row : s.all) {
                    if (row.tab == tab && !row.visible)
                        names.push_back(row.name + " (" + row.state + ", " + row.second + ")");
                }
                return joined(names, "; ");
            };
            differ(format("%s: %d hidden [%s], but %d after a fresh start [%s]", where.c_str(), hiddenA, hiddenIn(running).c_str(), hiddenB, hiddenIn(fresh).c_str()));
        }
        if (running.actionWords[tab] != fresh.actionWords[tab])
            differ(format("%s: X says %s, but %s after a fresh start", where.c_str(), running.actionWords[tab].c_str(), fresh.actionWords[tab].c_str()));

        // The same games in a different order. What's been done this time goes out of the way, which a fresh start doesn't know about.
        std::set<std::string> ready;
        for (auto* row : a) {
            if (row->state == "Ready")
                ready.insert(row->key);
        }
        std::vector<std::string> orderA, orderB;
        for (auto* row : a) {
            if (byKey.count(row->key) && ready.count(row->key))
                orderA.push_back(row->name);
        }
        for (auto* row : b) {
            if (ready.count(row->key))
                orderB.push_back(row->name);
        }
        if (orderA != orderB && tab == running.tab)
            differ(format("%s: in the order %s, but %s after a fresh start", where.c_str(), joined(orderA, ", ").c_str(), joined(orderB, ", ").c_str()));

        for (auto* row : a) {
            auto found = byKey.find(row->key);
            if (found == byKey.end())
                continue;
            const App::FullRow& f = *found->second;
            std::string name = where + ": '" + row->name + "'";
            // What happened in this run, such as a job that's done or failed, is only known to it.
            if (row->state != "Ready")
                continue;
            if (row->blocked != f.blocked)
                differ(name + format(" is %s, but %s after a fresh start", row->blocked ? "blocked" : "not blocked", f.blocked ? "blocked" : "not"));
            // The list that isn't on the screen gets analyzed once it's gone to.
            if (!row->hasEstimate && tab != running.tab && f.hasEstimate)
                continue;
            if (!row->hasEstimate && f.hasEstimate)
                differ(name + " isn't analyzed, but is after a fresh start");
            if (fromNothing) {
                if (row->hasEstimate && f.hasEstimate) {
                    i64 allowed = std::max<i64>(3 * std::max(row->marginBytes, f.marginBytes) + row->nowBytes / 100, 1 << 20);
                    if (std::llabs(row->afterBytes - f.afterBytes) > allowed)
                        differ(name + format(" comes to %s, but %s worked out from nothing", row->after.c_str(), f.after.c_str()));
                }
                if (withoutDigits(row->second) != withoutDigits(f.second))
                    differ(name + " says \"" + row->second + "\", but \"" + f.second + "\" worked out from nothing");
            } else {
                if (row->second != f.second)
                    differ(name + " says \"" + row->second + "\", but \"" + f.second + "\" after a fresh start");
                if (row->after != f.after || row->saved != f.saved || row->share != f.share)
                    differ(name + format(" shows %s / %s / %s, but %s / %s / %s after a fresh start", row->after.c_str(), row->saved.c_str(), row->share.c_str(), f.after.c_str(), f.saved.c_str(), f.share.c_str()));
            }
            if (row->status != f.status)
                differ(name + " shows \"" + row->status + "\", but \"" + f.status + "\" after a fresh start");
            if (!row->byHand && row->ticked != f.ticked)
                differ(name + format(" is %s (%s), but %s (%s) after a fresh start", row->ticked ? "ticked" : "unticked", row->second.c_str(), f.ticked ? "ticked" : "unticked", f.second.c_str()));
        }
    }
    std::string detail;
    for (size_t i = 0; i < differences.size() && i < 8; i++)
        detail += "\n      " + differences[i];
    report(step + (fromNothing ? ": worked out from nothing, it comes to the same" : ": a fresh start shows the same"), differences.empty(), detail);
}

/// What converting a row's files really comes to, with the settings it's shown with.
void Checker::checkSizes(const App::Snapshot& running, const std::string& step) {
    bool compress = running.actionWords[0] == "Compress";
    for (auto& row : running.all) {
        if (row.tab != 1 || !row.visible || row.state != "Ready" || row.blocked || !row.hasEstimate || !row.extracted.empty() || row.overInstalled)
            continue;
        // What's compressed already isn't converted again, whatever it would come to.
        if (compress && row.alreadyCompressed)
            continue;
        std::vector<std::string> files{row.mainFile};
        if (!row.updateFile.empty() && row.updateFile != row.mainFile)
            files.push_back(row.updateFile);
        for (auto& dlc : row.dlcFiles) {
            if (std::find(files.begin(), files.end(), dlc) == files.end())
                files.push_back(dlc);
        }
        std::string key = joined(files, "|") + (compress ? "|compressed" : "|repacked");
        for (auto& file : files)
            key += format("|%lld", (long long)sys::fileSize(file));
        if (!_realSizes.count(key)) {
            i64 total = 0;
            std::string made = _folder + "/check_size";
            sys::removeTree(made);
            sys::makeDirectories(made);
            try {
                TitleSet set(_keyset);
                for (auto& file : files)
                    set.add(std::make_shared<FileArchive>(file));
                set.group();
                ConvertOptions options;
                options.compress = compress;
                options.tempDir = made;
                options.threads = sys::workerCount();
                TitleConverter converter(options, _keyset);
                for (auto& job : set.jobs) {
                    NspOutput output(made + "/" + outputFileName(job, set, options), (int)job.contents.size(), 0, true);
                    converter.run(job, output);
                    total += output.outputBytes();
                }
                _foundSizes[key] = converter.foundBytes();
            } catch (const ToolError& e) {
                report(step + ": '" + row.name + "' can be converted, to see what it comes to", false, e.what());
                total = -1;
            }
            sys::removeTree(made);
            _realSizes[key] = total;
        }
        i64 real = _realSizes[key];
        if (real < 0)
            continue;
        // An estimate that read all of the game data has no guessing in it, only the padding between blocks taken as an average, which is
        // what tests/matrix.py holds small titles to. Regions that all came out the same say so too, which is only wrong if the rest differs.
        i64 allowed = row.marginBytes == 0 ? std::max<i64>(real / 200, 256 << 10) : std::max<i64>(3 * row.marginBytes + row.nowBytes / 50, 1 << 20);
        // What compressing found held again by itself isn't stored, which the estimate can't know: it can come to up to that much less.
        i64 found = _foundSizes.count(key) ? _foundSizes[key] : 0;
        i64 under = row.afterBytes - real;
        report(format("%s: '%s' comes to what its row says (%s)", step.c_str(), row.name.c_str(), compress ? "compressed" : "repacked"), under <= allowed + found && -under <= allowed,
               format("the row says %lld +/- %lld, it comes to %lld%s", (long long)row.afterBytes, (long long)row.marginBytes, (long long)real,
                      found > 0 ? format(", %lld found held again while compressing", (long long)found).c_str() : ""));
    }
}

/// A question that says something about the games it's asking about has to be right about them.
void Checker::checkQuestion(const App::Snapshot& asked, const std::string& step) {
    if (asked.dialog.find("no update") == std::string::npos)
        return;
    for (auto& row : asked.all) {
        if (row.tab != asked.tab || !row.visible || !row.ticked || asked.dialogText.find(row.name) == std::string::npos)
            continue;
        bool same = !row.hasEstimate || std::llabs(row.afterBytes - row.nowBytes) <= std::max<i64>(row.nowBytes / 50, 1 << 20);
        report(step + ": '" + row.name + "' is asked about as coming out the same size, and does", same,
               format("it's %s, and its row says %s", formatSize(row.nowBytes).c_str(), row.after.c_str()));
    }
}

/// Out of the settings, the menu, a question or the results, back to the list.
void Checker::backToList(Person& person, App& app) {
    for (int i = 0; i < 8; i++) {
        App::Snapshot s = app.snapshot();
        if (s.screen == "Main" && s.dialog.empty() && s.menu.empty())
            return;
        person.press(ButtonB);
        person.waitUntilIdle();
    }
}

Settings Checker::settingsOf(App& app) {
    std::string text = readText(_root + "/data/settings.txt");
    auto value = [&](const char* name) {
        size_t at = text.find(std::string("\n") + name + "=");
        if (at == std::string::npos)
            at = text.rfind(std::string(name) + "=", 0) == 0 ? 0 : std::string::npos;
        else
            at++;
        return at == std::string::npos ? 0 : atoi(text.c_str() + at + strlen(name) + 1);
    };
    (void)app;
    Settings s;
    s.compress = value("compress") != 0;
    s.installFiles = value("installgamefiles") != 0;
    s.hideDone = value("hidecompressed") != 0;
    s.minSaving = value("minimumsaving");
    s.sort = value("sort");
    s.thorough = value("measureall") != 0;
    return s;
}

/// One thing a person might do, chosen at random. Says what it was.
std::string Checker::step(Person& person, App& app, std::mt19937& random) {
    auto pick = [&](int n) { return (int)(random() % (unsigned)n); };
    App::Snapshot s = app.snapshot();
    int rows = (int)s.rows.size();
    switch (pick(12)) {
        case 0:
        case 1: {
            static const char* const options[] = {"Compression", "Game files and folders", "Minimum saving", "Hide games under 1 MB", "Analysis"};
            std::string option = options[pick(5)];
            int tab = s.tab;
            // Some only come with compressing.
            try {
                goToOption(person, app, option);
            } catch (const ToolError&) {
                goToTab(person, app, tab);
                return "there's no " + option + " now";
            }
            person.press(ButtonRight);
            std::string now = optionValue(app, option);
            goToTab(person, app, tab);
            return option + " changed to " + now;
        }
        case 2: {
            if (!goToMenuRow(person, app, "Sort by"))
                return "the menu had no Sort by";
            person.press(ButtonRight);
            backToList(person, app);
            return "sorted " + app.snapshot().sortedBy;
        }
        case 3:
            if (pick(2) == 0) {
                bool right = pick(2) == 0;
                person.press(right ? ButtonRight : ButtonLeft);
                return std::string(right ? "pressed right" : "pressed left") + " to show " + app.snapshot().views[s.tab < 0 ? 0 : s.tab];
            }
            goToTab(person, app, s.tab == 0 ? 1 : 0);
            return s.tab == 0 ? "went to Game files" : "went to Installed games";
        case 4:
        case 5: {
            if (rows == 0)
                return "nothing to tick";
            int down = pick(rows);
            for (int i = 0; i < down; i++)
                person.press(ButtonDown);
            person.press(ButtonA);
            return "ticked or unticked a game";
        }
        case 6:
            person.press(ButtonY);
            return "pressed Y";
        case 7: {
            // Asked to work a game out again, half the time from Done, which a game with no answer isn't under.
            if (s.tab < 0)
                return "nothing to analyze again on the overview";
            if (pick(2) == 0) {
                for (int i = 0; i < 2 && app.snapshot().views[s.tab] != "Done"; i++)
                    person.press(app.snapshot().views[s.tab] == "To do" ? ButtonRight : ButtonLeft);
            }
            std::string view = app.snapshot().views[s.tab];
            int shown = (int)app.snapshot().rows.size();
            if (shown == 0)
                return "nothing to analyze again under " + view;
            int down = pick(shown);
            for (int i = 0; i < down; i++)
                person.press(ButtonDown);
            static const char* const items[] = {"Analyze this game again", "Analyze the ticked games again", "Analyze every game again"};
            std::string item = items[pick(3)];
            if (!goToMenuRow(person, app, item)) {
                backToList(person, app);
                return "the menu had no " + item + " under " + view;
            }
            person.press(ButtonA);
            return "chose " + item + " under " + view;
        }
        case 8: {
            // Anything else in the menu that changes what's on the lists, from any view, going ahead with whatever it asks first. Never
            // deleting or uninstalling: the files given to the checker are the only copies.
            if (s.tab < 0)
                return "nothing from the menu on the overview";
            std::string view = s.views[s.tab];
            static const char* const items[] = {"Hide this game from the list", "Show the hidden games again", "Refresh the list", "Extract this game", "Install this game"};
            std::string item = items[pick(5)];
            bool job = item == "Extract this game" || item == "Install this game";
            if (job && _jobsRun >= 6)
                return "nothing run, enough has been";
            if (rows > 0) {
                int down = pick(rows);
                for (int i = 0; i < down; i++)
                    person.press(ButtonDown);
            }
            if (!goToMenuRow(person, app, item)) {
                backToList(person, app);
                return "the menu had no " + item + " under " + view;
            }
            person.press(ButtonA);
            std::string said = app.snapshot().dialog;
            for (int i = 0; i < 4 && !app.snapshot().dialog.empty(); i++)
                person.press(ButtonA);
            person.waitUntilIdle();
            _jobsRun += job ? 1 : 0;
            std::string result = app.snapshot().resultsTitle;
            backToList(person, app);
            return "chose " + item + " under " + view + (said.empty() ? "" : ", asked \"" + said + "\"") + (result.empty() ? "" : ", " + result);
        }
        default: {
            if (_jobsRun >= 6 || rows == 0)
                return "nothing run, enough has been";
            person.press(ButtonX);
            App::Snapshot asked = app.snapshot();
            checkQuestion(asked, "asked");
            std::string said = asked.dialog;
            // Whatever comes first: going ahead, or the first way on from a question about some of them.
            for (int i = 0; i < 4 && !app.snapshot().dialog.empty(); i++)
                person.press(ButtonA);
            person.waitUntilIdle();
            _jobsRun++;
            std::string result = app.snapshot().resultsTitle;
            backToList(person, app);
            return format("pressed X%s%s", said.empty() ? "" : (", asked \"" + said + "\"").c_str(), result.empty() ? "" : (", " + result).c_str());
        }
    }
}

/// Game files saved as NSPs: the compressed copy of a game that's installed as it was is done as a file, but can still be installed, which
/// the overview offers. X there installs just that, and the ticks on the list are as they were afterwards.
void Checker::checkInstallFromOverview() {
    Settings settings;
    settings.compress = true;
    settings.installFiles = false;
    settings.hideDone = true;
    setUp(settings);
    HostPlatform platform(_root, _keys, _console.get());
    App app(platform);
    app.start();
    Person person(app, _shots);
    person.looksAtOverview = true;
    person.waitUntilIdle();
    person.settle();
    App::Snapshot s = app.snapshot();
    person.shot("check_overview_save_mode");
    std::string lines;
    int at = -1;
    for (int i = 0; i < (int)s.overviewLines.size(); i++) {
        lines += (i ? "; " : "") + s.overviewLines[(size_t)i].text;
        if (s.overviewLines[(size_t)i].kind == "ToInstall")
            at = i;
    }
    report("saving NSPs, the overview offers to install the compressed file whose game isn't installed compressed", at >= 0 && s.overviewLines[(size_t)at].text == "1 compressed, not installed", lines);
    if (at < 0)
        return;
    std::map<std::string, bool> ticks;
    for (auto& row : s.all) {
        if (row.tab == 1)
            ticks[row.key] = row.ticked;
    }
    // Down from the line Installed games opens with goes onto the game under it, and right from there goes straight across, to what on
    // Game files is nearest in height.
    person.press(ButtonDown);
    App::Snapshot onGame = app.snapshot();
    if (onGame.overviewFocus >= 0 && onGame.overviewFocus < (int)onGame.overviewStops.size()) {
        App::Snapshot::Stop from = onGame.overviewStops[(size_t)onGame.overviewFocus];
        int nearest = -1, distance = 0;
        int dir = from.tab == 0 ? 1 : -1;
        // What's beside it in the same row on its own card first, as the two buttons at the bottom are.
        for (int i = 0; i < (int)onGame.overviewStops.size(); i++) {
            const App::Snapshot::Stop& stop = onGame.overviewStops[(size_t)i];
            bool sameRow = stop.y < from.y + from.h && from.y < stop.y + stop.h;
            int ahead = (stop.x + stop.w / 2 - (from.x + from.w / 2)) * dir;
            if (stop.tab == from.tab && sameRow && ahead > 0 && (nearest < 0 || ahead < distance)) {
                nearest = i;
                distance = ahead;
            }
        }
        for (int i = 0; nearest < 0 && i < (int)onGame.overviewStops.size(); i++) {
            const App::Snapshot::Stop& stop = onGame.overviewStops[(size_t)i];
            int away = stop.y + stop.h / 2 - (from.y + from.h / 2);
            away = away < 0 ? -away : away;
            if (stop.tab != from.tab && (nearest < 0 || away < distance)) {
                nearest = i;
                distance = away;
            }
        }
        person.press(from.tab == 0 ? ButtonRight : ButtonLeft);
        App::Snapshot across = app.snapshot();
        // The cards don't list games any more: down goes on down the card, and right from there straight across, all the same.
        report("down goes on down the card, and right from there straight across", nearest >= 0 && across.overviewFocus == nearest,
               format("from \"%s\", on %d, nearest %d", from.game.c_str(), across.overviewFocus, nearest));
    }
    // Up and down go through what's on a card, and left and right to the other card.
    auto stopOf = [&](const App::Snapshot& now) {
        for (int i = 0; i < (int)now.overviewStops.size(); i++) {
            if (now.overviewStops[(size_t)i].line == at && now.overviewStops[(size_t)i].kind == "Line")
                return i;
        }
        return -1;
    };
    for (int i = 0; i < 2 * (int)s.overviewStops.size() + 4; i++) {
        App::Snapshot now = app.snapshot();
        int focus = now.overviewFocus, wanted = stopOf(now);
        if (wanted < 0 || focus == wanted || focus < 0 || focus >= (int)now.overviewStops.size())
            break;
        int focusTab = now.overviewStops[(size_t)focus].tab, wantedTab = now.overviewStops[(size_t)wanted].tab;
        if (focusTab != wantedTab)
            person.press(focusTab < wantedTab ? ButtonRight : ButtonLeft);
        else
            person.press(focus < wanted ? ButtonDown : ButtonUp);
    }
    App::Snapshot reached = app.snapshot();
    int wanted = stopOf(reached);
    report("the D-pad gets to it on the overview", wanted >= 0 && reached.overviewFocus == wanted, format("on %d, wanted %d", reached.overviewFocus, wanted));
    if (wanted < 0 || reached.overviewFocus != wanted)
        return;
    person.press(ButtonA);
    std::string asked = app.snapshot().dialog;
    for (int i = 0; i < 4 && !app.snapshot().dialog.empty(); i++)
        person.press(ButtonA);
    person.waitUntilIdle();
    App::Snapshot done = app.snapshot();
    std::string results = joined(done.results, "; ");
    report("A on it installs that one file and nothing else, as it says", done.results.size() == 1, "asked \"" + asked + "\", " + results);
    person.shot("check_overview_installed");
    backToList(person, app);
    person.waitUntilIdle();
    person.settle();
    App::Snapshot after = app.snapshot();
    report("afterwards it's back on the overview", after.overview, after.screen);
    bool offered = false;
    for (auto& line : after.overviewLines)
        offered = offered || line.kind == "ToInstall";
    report("once it's installed, the overview no longer offers it", !offered);
    std::string changed;
    for (auto& row : after.all) {
        if (row.tab == 1 && ticks.count(row.key) && ticks[row.key] != row.ticked && row.state == "Ready")
            changed += row.name + "; ";
    }
    report("the ticks on Game files are as they were", changed.empty(), changed);
}

/// Everything under a folder, with its size, so what a job wrote can be told.
static std::map<std::string, i64> filesUnder(const std::string& folder) {
    std::map<std::string, i64> found;
    for (auto& entry : sys::list(folder)) {
        std::string path = folder + "/" + entry.name;
        if (entry.isDirectory) {
            for (auto& inside : filesUnder(path))
                found.insert(inside);
        } else {
            found[path] = entry.size;
        }
    }
    return found;
}

/// Compressing does only the ticked games. Thorough, its question says how many games a first look can't vouch for are still to be read in
/// full, as they may be ticked afterwards, and measuring goes on while it asks. Quick, those games aren't ticked, and compressing reads and
/// writes nothing for them, leaving them as they were. One ticked by hand is compressed whatever it saves, without being read in full first.
void Checker::checkMeasure() {
    // The highest minimum there is, which some of the test games save less than.
    Settings settings;
    settings.compress = true;
    settings.installFiles = false;
    settings.minSaving = 50;
    settings.thorough = false;
    setUp(settings);
    // Whether anything was read in full since the log said this, by analysis or by compressing.
    auto readInFullSince = [&](const std::string& before) {
        std::string log = readText(_root + "/data/log.txt");
        if (log.rfind(before, 0) == 0)
            log = log.substr(before.size());
        int count = 0;
        for (size_t at = log.find("Measur"); at != std::string::npos; at = log.find("Measur", at + 1))
            count++;
        return count;
    };
    HostPlatform platform(_root, _keys, _console.get());
    App app(platform);
    app.start();
    Person person(app, _shots);
    settleBothTabs(person, app, 1);
    std::vector<std::string> doubtful, names, tickedNames;
    std::string tickedNotSure;
    for (auto& row : app.snapshot().all) {
        if (row.tab != 1 || !row.visible || row.blocked)
            continue;
        if (row.ticked)
            tickedNames.push_back(row.name);
        if (!row.notSure)
            continue;
        doubtful.push_back(row.key);
        names.push_back(row.name);
        if (row.ticked)
            tickedNotSure += row.name + "; ";
    }
    report("quick, with a 50% minimum, game files a first look can't vouch for aren't ticked", !doubtful.empty() && tickedNotSure.empty() && !tickedNames.empty(),
           format("%d not sure, %d ticked, not sure and ticked: %s", (int)doubtful.size(), (int)tickedNames.size(), tickedNotSure.c_str()));
    if (doubtful.empty() || tickedNames.empty())
        return;
    person.shot("check_measure_list");

    // Thorough, chosen on the settings screen once every game has had its first look, the question says how many of them are still to be
    // measured, as they may be ticked afterwards, and measuring goes on while it asks. Slowly enough that none of them gets measured meanwhile,
    // and back to Quick, it stops.
    _putenv_s("HBP_TEST_SLOW_MEASURE", "60000");
    goToOption(person, app, "Analysis");
    person.press(ButtonRight);
    std::string setTo = optionValue(app, "Analysis");
    goToTab(person, app, 1);
    bool measuring = person.waitFor([&] { return app.snapshot().measuring; }, 120);
    person.press(ButtonX);
    person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 60);
    App::Snapshot thorough = app.snapshot();
    person.shot("check_measure_asked_thorough");
    report("thorough, compressing says how many that may save more are still to be measured, and measuring goes on while it asks",
           measuring && thorough.measuring && thorough.dialogText.find("still to be measured") != std::string::npos,
           format("measuring %d, then %d: ", measuring, thorough.measuring) + thorough.dialog + ": " + thorough.dialogText);
    person.press(ButtonB);
    goToOption(person, app, "Analysis");
    person.press(ButtonRight);
    std::string back = optionValue(app, "Analysis");
    goToTab(person, app, 1);
    bool stopped = person.waitFor([&] { return !app.snapshot().measuring; }, 15);
    _putenv_s("HBP_TEST_SLOW_MEASURE", "");
    report("back to Quick, measuring stops", setTo == "Thorough" && back == "Quick" && stopped, setTo + " then " + back + format(", stopped %d", stopped));
    person.waitUntilIdle();

    std::map<std::string, i64> beforeOutput = filesUnder(_root + "/output"), beforeTemp = filesUnder(_root + "/data/temp");
    std::string logBefore = readText(_root + "/data/log.txt");
    person.press(ButtonX);
    person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 60);
    App::Snapshot asked = app.snapshot();
    report("quick, compressing asks about the ticked games, saying nothing of the ones that may save more",
           asked.dialog.rfind("Compress th", 0) == 0 && asked.dialogText.find("may save more") == std::string::npos, asked.dialog + ": " + asked.dialogText);
    person.shot("check_measure_asked");
    for (int i = 0; i < 4 && !app.snapshot().dialog.empty() && app.snapshot().screen == "Main"; i++)
        person.press(ButtonA);
    person.waitFor([&] { return app.snapshot().screen == "Results"; }, 600);
    App::Snapshot done = app.snapshot();
    person.shot("check_measure_results");
    auto inResults = [](const App::Snapshot& s, const std::string& name) {
        return std::any_of(s.results.begin(), s.results.end(), [&](const std::string& r) { return r.rfind(name + ": ", 0) == 0; });
    };
    std::string missing, included;
    for (auto& name : tickedNames)
        missing += inResults(done, name) ? "" : name + "; ";
    for (auto& name : names)
        included += inResults(done, name) ? name + "; " : "";
    report("it does the ticked games, and none of the ones that may save more", done.screen == "Results" && missing.empty() && included.empty(),
           "missing: " + missing + " included: " + included + " / " + joined(done.results, "; "));
    // Nothing is read in full or written for them, wherever games go and wherever work in progress does.
    std::string written;
    for (auto& file : filesUnder(_root + "/output")) {
        for (auto& name : names) {
            if (!beforeOutput.count(file.first) && file.first.find(name) != std::string::npos)
                written += file.first + "; ";
        }
    }
    for (auto& file : filesUnder(_root + "/data/temp")) {
        if (!beforeTemp.count(file.first))
            written += file.first + "; ";
    }
    int read = readInFullSince(logBefore);
    report("and nothing is read in full or written for them", written.empty() && read == 0, written + format("%d read in full", read));
    backToList(person, app);
    person.waitUntilIdle();
    std::string changed;
    for (auto& row : app.snapshot().all) {
        if (std::find(doubtful.begin(), doubtful.end(), row.key) != doubtful.end() && (!row.notSure || row.ticked || row.measured || row.inDone))
            changed += format("%s: not sure %d, ticked %d, measured %d, done %d; ", row.name.c_str(), row.notSure, row.ticked, row.measured, row.inDone);
    }
    report("afterwards they're as they were, not sure yet and unticked", changed.empty(), changed);

    // Ticked by hand, one of them is compressed whatever it saves, without being read in full first.
    goToTab(person, app, 1);
    for (int i = 0; i < 4; i++) {
        bool any = false;
        for (auto& row : app.snapshot().rows)
            any = any || row.ticked;
        if (!any)
            break;
        person.press(ButtonY);
    }
    person.waitUntilIdle();
    auto rows = app.snapshot().rows;
    int index = -1;
    for (int i = 0; i < (int)rows.size() && index < 0; i++)
        index = std::find(names.begin(), names.end(), rows[(size_t)i].name) != names.end() ? i : -1;
    std::string chosen = index >= 0 ? rows[(size_t)index].name : "";
    for (int i = 0; i < (int)rows.size() && index >= 0; i++)
        person.press(ButtonUp);
    for (int i = 0; i < index; i++)
        person.press(ButtonDown);
    if (index >= 0)
        person.press(ButtonA);
    std::string tickedNow;
    for (auto& row : app.snapshot().rows)
        tickedNow += row.ticked ? row.name + "; " : "";
    if (index < 0 || tickedNow != chosen + "; ") {
        report("one that may save more can be ticked by hand", false, format("%d rows, ticked: ", (int)rows.size()) + tickedNow);
        return;
    }
    logBefore = readText(_root + "/data/log.txt");
    person.press(ButtonX);
    person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 60);
    asked = app.snapshot();
    for (int i = 0; i < 4 && !app.snapshot().dialog.empty() && app.snapshot().screen == "Main"; i++)
        person.press(ButtonA);
    person.waitFor([&] { return app.snapshot().screen == "Results"; }, 600);
    done = app.snapshot();
    person.shot("check_measure_by_hand");
    bool saved = std::any_of(done.results.begin(), done.results.end(), [&](const std::string& r) { return r.rfind(chosen + ": Saved as ", 0) == 0; });
    read = readInFullSince(logBefore);
    report("ticked by hand, one of them is compressed whatever it saves, without being read in full first", asked.dialog == "Compress this game?" && saved && read == 0,
           asked.dialog + " / " + joined(done.results, "; ") + format(" / %d read in full", read));
    backToList(person, app);
}

/// Thorough, analyzing goes on by itself to read in full what a first look can't vouch for, writing nothing but what it found, so each game
/// says what it saves, ticked if that's worth it. What it found is kept, and not read again.
void Checker::checkThorough() {
    Settings settings;
    settings.compress = true;
    settings.installFiles = false;
    settings.minSaving = 50;
    settings.thorough = false;
    setUp(settings);
    // How many games were read in full since the log said this, whether it goes on from it or started again.
    auto measuringsSince = [&](const std::string& before) {
        std::string log = readText(_root + "/data/log.txt");
        if (log.rfind(before, 0) == 0)
            log = log.substr(before.size());
        int count = 0;
        for (size_t at = log.find("Measuring "); at != std::string::npos; at = log.find("Measuring ", at + 1))
            count++;
        return count;
    };
    std::map<std::string, i64> before = filesUnder(_root + "/output"), beforeTemp = filesUnder(_root + "/data/temp");
    int measured = 0;
    {
        HostPlatform platform(_root, _keys, _console.get());
        App app(platform);
        app.start();
        Person person(app, _shots);
        settleBothTabs(person, app, 1);
        // Quick to begin with: what a first look can't vouch for waits for Measure.
        int waiting = 0;
        for (auto& row : app.snapshot().all)
            waiting += row.tab == 1 && row.visible && row.hasEstimate && !row.inDone && row.notSure ? 1 : 0;
        report("quick, what a first look can't vouch for waits, not sure yet", waiting > 0, format("%d not sure", waiting));
        // Chosen on the settings screen, as a person would.
        goToOption(person, app, "Analysis");
        person.press(ButtonRight);
        std::string chosen = optionValue(app, "Analysis");
        goToTab(person, app, 1);
        person.waitUntilIdle();
        report("Analysis is set to Thorough on the settings screen", chosen == "Thorough", chosen);
        std::string wrong;
        for (auto& row : app.snapshot().all) {
            // In whichever view, but not what's compressed already, which there's nothing to measure of.
            if (row.tab != 1 || !(row.inToDo || row.inDone) || !row.hasEstimate || row.alreadyCompressed)
                continue;
            if (row.notSure) {
                wrong += row.name + ": still not sure; ";
                continue;
            }
            if (!row.measured)
                continue;
            measured++;
            // Worth it, it's to do and ticked, with its figure. One its developer compressed is to do as well, but only ever ticked by hand, as
            // its updates then need its original, which its line says. Otherwise it's settled: under Done, unticked, saying why.
            bool developer = row.second.rfind("Compressed by its developer", 0) == 0;
            bool worthIt = (developer ? !row.ticked || row.byHand : row.ticked) && row.inToDo && row.saved != "Not sure" && !row.saved.empty();
            bool out = !row.ticked && row.inDone && (row.second.rfind("Not worth it", 0) == 0 || row.second.rfind("Nothing to gain", 0) == 0);
            if (!worthIt && !out)
                wrong += format("%s: ticked %d, to do %d, saves \"%s\", \"%s\"; ", row.name.c_str(), row.ticked, row.inToDo, row.saved.c_str(), row.second.c_str());
        }
        int left = 0;
        for (auto& row : app.snapshot().all)
            left += row.unmeasured ? 1 : 0;
        report("thorough, every game is read in full, not only the ones a first look couldn't vouch for", left == 0, format("%d not read in full", left));
        report("thorough, what a first look couldn't vouch for is measured by itself, and judged on that", wrong.empty() && measured > 0,
               wrong + format("%d measured", measured));
        person.shot("check_thorough_list");
        std::string written;
        for (auto& file : filesUnder(_root + "/output")) {
            if (!before.count(file.first) || before[file.first] != file.second)
                written += file.first + "; ";
        }
        for (auto& file : filesUnder(_root + "/data/temp")) {
            if (!beforeTemp.count(file.first))
                written += file.first + "; ";
        }
        report("and nothing is written", written.empty(), written);
        backToList(person, app);
    }

    // Opened again with a lower minimum, what was measured is kept and judged again on it, without reading any of it again: those that save
    // that much are ticked with their figures, and the rest say why they're left out.
    settings.thorough = true;
    settings.minSaving = 5;
    writeSettings(settings);
    std::string logBefore = readText(_root + "/data/log.txt");
    HostPlatform platform(_root, _keys, _console.get());
    App app(platform);
    app.start();
    Person person(app, _shots);
    settleBothTabs(person, app, 1);
    person.waitUntilIdle();
    int kept = 0;
    std::string wrong;
    for (auto& row : app.snapshot().all) {
        if (row.tab != 1 || !(row.inToDo || row.inDone) || !row.hasEstimate || row.alreadyCompressed || !row.measured || row.notSure)
            continue;
        kept++;
        // One its developer compressed is to do, but only ever ticked by hand (as above).
        bool developer = row.second.rfind("Compressed by its developer", 0) == 0;
        bool shows = (developer ? !row.ticked || row.byHand : row.ticked) && row.inToDo && row.saved != "Not sure" && !row.saved.empty();
        bool out = !row.ticked && row.inDone && (row.second.rfind("Not worth it", 0) == 0 || row.second.rfind("Nothing to gain", 0) == 0);
        if (!shows && !out)
            wrong += format("%s: ticked %d, to do %d, saves \"%s\", \"%s\"; ", row.name.c_str(), row.ticked, row.inToDo, row.saved.c_str(), row.second.c_str());
    }
    int again = measuringsSince(logBefore);
    report("opened again with a 5% minimum, what was measured is kept and judged again on it, not measured again", kept == measured && again == 0 && wrong.empty(),
           wrong + format("%d measured, %d kept, %d measured again", measured, kept, again));
    person.shot("check_measure_kept");
    backToList(person, app);
}

/// The questions asked once, when they first matter, and - on a list: measuring what a first look can't vouch for, now or by itself from then
/// on; measuring by itself keeping each game's answer as it's found, and stopping when Quick is chosen again; deleting files once they're
/// installed; and the console, when the SD card hasn't room.
void Checker::checkOffers() {
    auto notSureLeft = [](App& app) {
        int left = 0;
        for (auto& row : app.snapshot().all)
            left += row.hasEstimate && row.notSure && !row.blocked ? 1 : 0;
        return left;
    };
    auto dialogStarts = [](App& app, const std::string& start) { return app.snapshot().dialog.rfind(start, 0) == 0; };
    // Nothing ticked on the list, and then the one game.
    auto tickOnly = [](Person& person, App& app, const std::string& name) {
        for (int i = 0; i < 4; i++) {
            bool any = false;
            for (auto& row : app.snapshot().rows)
                any = any || row.ticked;
            if (!any)
                break;
            person.press(ButtonY);
        }
        auto rows = app.snapshot().rows;
        int index = -1;
        for (int i = 0; i < (int)rows.size() && index < 0; i++)
            index = rows[(size_t)i].name.find(name) != std::string::npos ? i : -1;
        if (index < 0)
            return false;
        for (int i = 0; i < (int)rows.size(); i++)
            person.press(ButtonUp);
        for (int i = 0; i < index; i++)
            person.press(ButtonDown);
        person.press(ButtonA);
        return true;
    };
    auto runJob = [&](Person& person, App& app) {
        for (int i = 0; i < 4 && !app.snapshot().dialog.empty() && app.snapshot().screen == "Main"; i++)
            person.press(ButtonA);
        return person.waitFor([&] { return app.snapshot().screen == "Results"; }, 600);
    };

    // Across to a list's card on the overview, and down it to a line of that kind.
    auto pointAtLine = [](Person& person, App& app, int tab, const std::string& kind) {
        for (int i = 0; i < 30; i++) {
            App::Snapshot s = app.snapshot();
            if (s.overviewFocus < 0 || s.overviewFocus >= (int)s.overviewStops.size())
                return false;
            const App::Snapshot::Stop& stop = s.overviewStops[(size_t)s.overviewFocus];
            if (stop.kind == "Line" && stop.tab == tab && stop.line >= 0 && stop.line < (int)s.overviewLines.size() && s.overviewLines[(size_t)stop.line].kind == kind)
                return true;
            person.press(stop.tab != tab ? (tab > stop.tab ? ButtonRight : ButtonLeft) : ButtonDown);
        }
        return false;
    };

    // Quick to begin with, Thorough set in Settings reads every game in full by itself, keeping each answer as soon as it's found, and back to
    // Quick it stops at once, without starting again.
    Settings settings;
    settings.compress = true;
    settings.installFiles = false;
    settings.minSaving = 50;
    settings.thorough = false;
    setUp(settings);
    _putenv_s("HBP_TEST_SLOW_MEASURE", "3000");
    {
        HostPlatform platform(_root, _keys, _console.get());
        App app(platform);
        app.start();
        Person person(app, _shots);
        settleBothTabs(person, app, 1);
        int waiting = notSureLeft(app);
        goToOption(person, app, "Analysis");
        person.press(ButtonRight);
        std::string chosen = optionValue(app, "Analysis");
        goToTab(person, app, 1);
        bool measuring = person.waitFor([&] { return app.snapshot().measuring; }, 120);
        bool first = person.waitFor([&] { return notSureLeft(app) < waiting; }, 120);
        // Each answer is kept as soon as it's found: a game read in full took minutes.
        int saved = 0;
        std::string answers = readText(_root + "/data/analysis.txt");
        for (size_t at = 0; at < answers.size();) {
            size_t end = std::min(answers.find('\n', at), answers.size());
            std::string line = answers.substr(at, end - at);
            at = end + 1;
            size_t tab = line.find('\t');
            long long input, output, margin, romfs, stored;
            int compressed, measured = 0;
            if (tab != std::string::npos && sscanf(line.c_str() + tab + 1, "%lld %lld %lld %lld %lld %d %d", &input, &output, &margin, &romfs, &stored, &compressed, &measured) == 7)
                saved += measured;
        }
        report("Thorough, set in Settings, measures by itself, keeping each answer as soon as it's found", chosen == "Thorough" && measuring && first && saved > 0,
               format("%s, measuring %d, first measured %d, %d kept", chosen.c_str(), measuring, first, saved));

        goToOption(person, app, "Analysis");
        person.press(ButtonRight);
        chosen = optionValue(app, "Analysis");
        goToTab(person, app, 1);
        bool stopped = person.waitFor([&] { return !app.snapshot().measuring; }, 15);
        // It doesn't start again by itself.
        bool again = person.waitFor([&] { return app.snapshot().measuring; }, 8);
        int left = notSureLeft(app);
        report("back to Quick, measuring stops at once and doesn't start again", chosen == "Quick" && stopped && !again && left > 0,
               format("%s, stopped %d, started again %d, %d still not sure", chosen.c_str(), stopped, again, left));
    }

    // Thorough, as it is to begin with, the overview has a line saying how far measuring has got. A on it says what it is, with the way to
    // turn it off, which stops it at once.
    settings.thorough = true;
    setUp(settings);
    {
        HostPlatform platform(_root, _keys, _console.get());
        App app(platform);
        app.start();
        Person person(app, _shots);
        bool measuring = person.waitFor([&] { return app.snapshot().measuring; }, 300);
        for (int i = 0; i < 3 && !app.snapshot().overview; i++)
            person.press(ButtonL);
        int tab = -1;
        std::string lineText;
        for (auto& line : app.snapshot().overviewLines) {
            if (line.kind == "NotSure" && tab < 0) {
                tab = line.tab;
                lineText = line.text;
            }
        }
        bool pointed = tab >= 0 && pointAtLine(person, app, tab, "NotSure");
        person.shot("check_overview_measuring");
        person.press(ButtonA);
        bool asked = person.waitFor([&] { return app.snapshot().dialog == "Measuring for exact figures"; }, 10);
        App::Snapshot s = app.snapshot();
        report("thorough, the overview says how far measuring has got, and A on it says what it is",
               measuring && pointed && lineText.rfind("Measuring for exact figures", 0) == 0 && asked && s.dialogText.find("Analysis, in Settings") != std::string::npos,
               format("measuring %d, pointed %d: ", measuring, pointed) + lineText + " / " + s.dialog + ": " + s.dialogText);
        person.shot("check_overview_measuring_said");
        if (asked) {
            person.press(ButtonRight);
            person.press(ButtonA);
        }
        bool stopped = person.waitFor([&] { return !app.snapshot().measuring; }, 15);
        bool again = person.waitFor([&] { return app.snapshot().measuring; }, 8);
        report("turning it off there stops it at once, and it stays off", asked && stopped && !again &&
               readText(_root + "/data/settings.txt").find("measureall=0") != std::string::npos, format("stopped %d, started again %d", stopped, again));
    }
    _putenv_s("HBP_TEST_SLOW_MEASURE", "");

    // The first install that keeps its files asks the once whether to delete them, with A keeping them.
    settings = Settings();
    settings.compress = true;
    settings.installFiles = true;
    settings.deleteAsked = false;
    setUp(settings);
    std::vector<std::string> files;
    for (auto& file : filesUnder(_root + "/sd/games")) {
        if (file.first.find("Repeats Test") != std::string::npos)
            files.push_back(file.first);
    }
    {
        HostPlatform platform(_root, _keys, _console.get());
        App app(platform);
        app.start();
        Person person(app, _shots);
        settleBothTabs(person, app, 1);
        bool ticked = tickOnly(person, app, "Repeats Test");
        person.press(ButtonX);
        person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 60);
        bool finished = ticked && runJob(person, app);
        bool asked = person.waitFor([&] { return dialogStarts(app, "Delete the"); }, 30);
        App::Snapshot s = app.snapshot();
        report("after the first install that kept its files, it asks the once whether to delete them", finished && asked && s.dialogText.find("Once a file is installed") != std::string::npos,
               format("ticked %d, finished %d: ", ticked, finished) + s.dialog + ": " + s.dialogText);
        person.shot("check_offer_delete");
        if (asked) {
            person.press(ButtonLeft);
            person.press(ButtonLeft);
            person.press(ButtonA);
        }
        person.waitUntilIdle();
        std::string left;
        for (auto& file : files)
            left += sys::exists(file) ? file + "; " : "";
        report("deleting them deletes the game's files, and it isn't asked again", asked && !files.empty() && left.empty() &&
               readText(_root + "/data/settings.txt").find("deleteasked=1") != std::string::npos, format("%d files, left: %s", (int)files.size(), left.c_str()));
        backToList(person, app);
    }

    // With no room on the SD card, and room on the console, it asks whether to install there instead, this time.
    settings.deleteAsked = true;
    setUp(settings);
    _console->setFree(Storage::Sd, 1 << 20);
    {
        HostPlatform platform(_root, _keys, _console.get());
        App app(platform);
        app.start();
        Person person(app, _shots);
        settleBothTabs(person, app, 1);
        bool ticked = tickOnly(person, app, "Repeats Test");
        person.press(ButtonX);
        bool asked = person.waitFor([&] { return app.snapshot().dialog == "Install to the console instead?"; }, 60);
        report("with no room on the SD card, it asks whether to install to the console instead", ticked && asked, app.snapshot().dialog + ": " + app.snapshot().dialogText);
        person.shot("check_offer_console");
        if (asked) {
            person.press(ButtonA);
            bool says = person.waitFor([&] { return dialogStarts(app, "Install") && app.snapshot().dialogText.find("on the console") != std::string::npos; }, 30);
            report("and then the install question says the console", says, app.snapshot().dialog + ": " + app.snapshot().dialogText);
            bool finished = runJob(person, app);
            backToList(person, app);
            goToTab(person, app, 0);
            person.waitUntilIdle();
            for (int i = 0; i < 4 && app.snapshot().views[0] != "Everything"; i++)
                person.press(ButtonRight);
            std::string where;
            for (auto& row : app.snapshot().rows)
                where += row.name.find("Repeats Test") != std::string::npos ? row.detail : "";
            report("it goes on the console, and Install games to stays as it was", finished && where.find("On the console") != std::string::npos &&
                   readText(_root + "/data/settings.txt").find("installto=1") == std::string::npos, format("finished %d: %s", finished, where.c_str()));
        }
        backToList(person, app);
    }
}

/// A copy made by a version from before 1.4, which wrote its game data after the levels that check it, is this tool's all the same: installed,
/// it's an older copy, saying what it freed, and never one its makers compressed. Only with HBP_TEST_OLD_COPY, a file such a version made.
void Checker::checkVeryOldCopy() {
    const char* path = getenv("HBP_TEST_OLD_COPY");
    if (!path || !*path)
        return;
    Settings settings;
    settings.compress = true;
    settings.installFiles = true;
    setUp(settings);
    _console->install({path}, Storage::Sd);
    HostPlatform platform(_root, _keys, _console.get());
    App app(platform);
    app.start();
    Person person(app, _shots);
    settleBothTabs(person, app, 0);
    for (int i = 0; i < 4 && app.snapshot().views[0] != "Everything"; i++)
        person.press(ButtonRight);
    App::Snapshot s = app.snapshot();
    const App::FullRow* old = nullptr;
    std::string names;
    for (auto& row : s.all) {
        if (row.tab != 0)
            continue;
        names += row.name + "; ";
        if (row.name.find("LZ Test") != std::string::npos)
            old = &row;
    }
    report("a copy made before 1.4 is installed", old != nullptr, names);
    if (!old)
        return;
    person.shot("check_very_old_copy");
    report("it's an older copy, saying what it freed, not one its developer compressed",
           old->olderCopy && !old->alreadyCompressed && old->freedBytes > 0 && old->second.find("developer") == std::string::npos,
           format("older copy %d, compressed %d, freed %lld, \"%s\"", old->olderCopy, old->alreadyCompressed, (long long)old->freedBytes, old->second.c_str()));
    report("and it's under Done, unless compressing it again is sure to be worth it", old->inDone || (old->inToDo && old->ticked && !old->notSure),
           format("done %d, to do %d, ticked %d, not sure %d", old->inDone, old->inToDo, old->ticked, old->notSure));
    backToList(person, app);
}

/// A copy this makes says what it freed, and one an older version made comes up again on the console, as made again it can come out smaller:
/// it's made from the copy itself, and what that makes says this version made it, and what the game came to before it was ever compressed.
void Checker::checkCopies() {
    Settings settings;
    settings.compress = true;
    settings.installFiles = true;
    setUp(settings);
    auto installedRow = [&](App& app) -> const App::FullRow* {
        static App::Snapshot kept;
        kept = app.snapshot();
        for (auto& row : kept.all) {
            if (row.tab == 0)
                return &row;
        }
        return nullptr;
    };
    // Everything on Installed games, wherever a copy is, so the one game is in front of the pointer.
    auto showEverything = [&](Person& person, App& app) {
        for (int i = 0; i < 4 && app.snapshot().views[0] != "Everything"; i++)
            person.press(ButtonRight);
    };
    auto compressTicked = [&](Person& person, App& app) {
        person.press(ButtonX);
        person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 30);
        for (int i = 0; i < 4 && !app.snapshot().dialog.empty(); i++)
            person.press(ButtonA);
        person.waitUntilIdle();
        App::Snapshot done = app.snapshot();
        backToList(person, app);
        person.waitUntilIdle();
        return done;
    };

    // Compressed as a version from before 1.5 would have, which says so in what it makes.
    _putenv_s("HBP_TEST_MADE_BY", "1.4.3");
    i64 firstFreed = 0;
    {
        HostPlatform platform(_root, _keys, _console.get());
        App app(platform);
        app.start();
        Person person(app, _shots);
        settleBothTabs(person, app, 0);
        if (app.snapshot().tickHint == "Tick all")
            person.press(ButtonY);
        App::Snapshot done = compressTicked(person, app);
        report("an installed game is compressed, as an older version would have", done.resultsTitle == "Finished" && !done.results.empty(), done.resultsTitle + ": " + joined(done.results, "; "));
    }
    _putenv_s("HBP_TEST_MADE_BY", "");
    // What this version remembers of a copy it made itself is that it's done. An older version's copy comes with nothing like that.
    for (const char* where : {"/data/", "/config/"})
        sys::removeFile(_root + where + "analysis.txt");
    // Opened again, it's looked at like any other game, and says what compressing it freed.
    {
        HostPlatform platform(_root, _keys, _console.get());
        App app(platform);
        app.start();
        Person person(app, _shots);
        settleBothTabs(person, app, 0);
        const App::FullRow* row = installedRow(app);
        // Unless a first look is sure it's worth compressing again, it waits under Done, as it was, saying what it freed.
        report("a copy an older version made stays under Done until it's worth compressing again", row && (row->inDone || (row->inToDo && row->ticked && !row->notSure)),
               row ? format("done %d, to do %d, ticked %d, not sure %d", row->inDone, row->inToDo, row->ticked, row->notSure) : "no installed game");
        report("a copy an older version made comes up to be looked at again, saying what it freed", row && row->olderCopy && !row->alreadyCompressed && row->freedBytes > 0,
               row ? format("%s: older %d, compressed %d, freed %lld, \"%s\"", row->name.c_str(), row->olderCopy, row->alreadyCompressed, (long long)row->freedBytes, row->second.c_str())
                   : "no installed game");
        person.shot("check_older_copy");
        if (!row || !row->olderCopy)
            return;
        firstFreed = row->freedBytes;
        // Ticked by hand, it's made again from the copy itself.
        showEverything(person, app);
        if (!installedRow(app)->ticked)
            person.press(ButtonA);
        App::Snapshot done = compressTicked(person, app);
        report("made again from the copy itself, it's compressed", done.resultsTitle == "Finished" && done.results.size() == 1 && done.results[0].find("Now ") != std::string::npos,
               done.resultsTitle + ": " + joined(done.results, "; "));
    }
    // And what that made says this version made it, and what the game came to before it was ever compressed.
    HostPlatform platform(_root, _keys, _console.get());
    App app(platform);
    app.start();
    Person person(app, _shots);
    settleBothTabs(person, app, 0);
    const App::FullRow* row = installedRow(app);
    report("made again, it's compressed as this version makes it, saying what it freed from the start", row && row->alreadyCompressed && !row->olderCopy &&
           row->freedBytes >= firstFreed && row->second.rfind("Compressed, ", 0) == 0 && row->second.find(" freed") != std::string::npos,
           row ? format("compressed %d, older %d, freed %lld then %lld, \"%s\"", row->alreadyCompressed, row->olderCopy, (long long)firstFreed, (long long)row->freedBytes, row->second.c_str())
               : "no installed game");
    person.shot("check_copy_again");
}

/// Every game file's estimate, compressed and repacked, against what converting it really comes to, before anything else, as the steps only come
/// to it now and then.
void Checker::checkEveryEstimate() {
    for (bool compress : {true, false}) {
        Settings settings;
        settings.compress = compress;
        setUp(settings);
        HostPlatform platform(_root, _keys, _console.get());
        App app(platform);
        app.start();
        Person person(app, _shots);
        person.looksAtOverview = true;
        settleBothTabs(person, app, 1);
        checkSizes(app.snapshot(), compress ? "at the start, compressed" : "at the start, repacked");
    }
}

int Checker::run(unsigned seed, int steps) {
    std::mt19937 random(seed);
    Settings start;
    start.compress = random() % 2 == 0;
    start.installFiles = random() % 2 == 0;
    start.hideDone = random() % 2 == 0;
    start.sort = (int)(random() % 3);
    if (seed == 1) {
        checkInstallFromOverview();
        checkOffers();
    }
    if (seed == 2)
        checkMeasure();
    if (seed == 3) {
        checkCopies();
        checkVeryOldCopy();
    }
    if (seed == 4)
        checkThorough();
    checkEveryEstimate();
    printf("Seed %u, starting with %s\n", seed, describe(start).c_str());
    setUp(start);

    HostPlatform platform(_root, _keys, _console.get());
    App app(platform);
    app.start();
    Person person(app, _shots);
    person.looksAtOverview = true;
    person.waitUntilIdle();
    person.settle();
    report("it opens on the overview", app.snapshot().overview, app.snapshot().screen);
    goToTab(person, app, (int)(random() % 2));
    person.waitUntilIdle();

    for (int i = 1; i <= steps; i++) {
        std::string did = step(person, app, random);
        person.waitUntilIdle();
        backToList(person, app);
        person.waitUntilIdle();
        person.settle();
        std::string name = format("step %d (%s; %s)", i, did.c_str(), describe(settingsOf(app)).c_str());
        printf("%s\n", name.c_str());
        App::Snapshot running = app.snapshot();
        compare(running, freshStart(running, false, format("check_%u_%02d_fresh", seed, i)), name, false);
        if (i % 4 == 0)
            compare(running, freshStart(running, true, format("check_%u_%02d_cold", seed, i)), name, true);
        checkSizes(running, name);
        checkOverview(person, app, name);
        person.shot(format("check_%u_%02d", seed, i));
    }
    return g_failures > 0 ? 1 : 0;
}

}  // namespace

int runCheck(const std::string& keys, const std::string& folder, const std::string& shots, unsigned seed, int steps, const std::vector<std::string>& nsps) {
    if (nsps.size() < 4) {
        printf("check needs base.nsp update.nsp dlc.nsp other.nsp, and then any more game files\n");
        return 2;
    }
    Checker checker(keys, folder, shots, nsps);
    return checker.run(seed, steps);
}

}  // namespace apptest
