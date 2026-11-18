// Runs the whole app on a PC. A pretend person presses the buttons, the screen is saved as pictures to look at, and what became of the
// pretend console is checked afterwards.
//   app_test -k keys folder shots demo
//   app_test -k keys folder shots real base.nsp update.nsp [other.nsp]
//   app_test -k keys folder shots check [seed] [steps] base.nsp update.nsp dlc.nsp other.nsp [more...]   (see app_check.cpp)
//
// With UPDATE_DIR set to a folder holding what tools\sign_release.py made for a program.bin of version 9.9.9, updating gets tried too.
#include <winsock2.h>
#include <windows.h>
#include <psapi.h>
#include <wincodec.h>

#pragma comment(lib, "psapi.lib")

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

#include "../app/app.h"
#include "../app/app_internal.h"
#include "../core/remote.h"
#include "app_test_support.h"
#include "sim_console.h"

using namespace hbp;
using namespace apptest;

namespace apptest {
int runCheck(const std::string& keys, const std::string& folder, const std::string& shots, unsigned seed, int steps, const std::vector<std::string>& nsps);
}

namespace {

/// Plenty of made-up games with made-up answers, for looking at how the screens fill up.
class DemoConsole : public ConsoleApi {
public:
    struct Game {
        const char* name;
        const char* version;
        double gigabytes;
        int dlc;
        double ratio;
        bool icon;
        const char* blocked;
    };

    static const std::vector<Game>& games() {
        static const std::vector<Game> list = many({
            {"The Legend of the Hollow Crown: Tears of Time and Space, Definitive Collector's Edition", "1.2.1", 16.2, 2, 0.41, true, ""},
            {"Stardew Acres", "1.6.8", 0.48, 0, 0.32, true, ""},
            {"Metro Exodus Redux", "1.0.2", 27.4, 3, 0.18, true, ""},
            {"Hollow Songs", "1.5.78", 6.9, 0, 0.27, true, ""},
            {"Super Kart Racing Deluxe", "3.0.1", 13.8, 2, 0.06, false, ""},
            {"Celestial Peak", "1.4.0", 0.96, 0, 0.12, true, ""},
            {"Portal Knights Remastered", "2.1.0", 5.3, 1, 0.44, true, ""},
            {"Dark Chronicles IV", "1.0.5", 31.2, 0, 0.02, false, ""},
            {"A Short Stroll", "1.3.0", 0.3, 0, 0.14, true, ""},
            {"Tiny Tunes Adventure", "1.1.0", 2.2, 0, 0.51, true, ""},
            {"Fire Saga: Three Banners", "1.8.0", 14.9, 4, 0.22, true, "hacBrewPackLZ is open through this game: open it from another one"},
            {"Animal Gardens", "2.0.0", 11.1, 1, 0.0, true, ""},
            {"Rogue Cells", "3.3.0", 1.1, 3, 0.09, false, ""},
            {"Neon Drift Racing", "2.4.1", 4.6, 0, -0.04, true, ""},
        });
        return list;
    }

    /// The games over again, HBP_TEST_DEMO_MANY times in all, each copy named apart, for timing a list as long as a full console's.
    static std::vector<Game> many(std::vector<Game> base) {
        const char* times = getenv("HBP_TEST_DEMO_MANY");
        int count = times ? std::max(1, atoi(times)) : 1;
        static std::vector<std::string> names;
        names.reserve(base.size() * (size_t)count);
        std::vector<Game> all;
        for (int k = 0; k < count; k++) {
            for (Game game : base) {
                if (k > 0) {
                    names.push_back(std::string(game.name) + " " + std::to_string(k + 1));
                    game.name = names.back().c_str();
                }
                all.push_back(game);
            }
        }
        return all;
    }

    static u64 idOf(size_t index) { return 0x0100000000010000ull + ((u64)index << 16); }

    static std::string fingerprintOf(const InstalledGame& game) {
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

    /// An answer as the app writes it, with compression on unless the settings say level 0 and block size 0, which is how it says it's off.
    static std::string answerFor(DemoConsole& console, size_t index, double ratio, bool compressed, const std::string& generation = hbp::app::AnswerGeneration,
                                 const char* settings = "9|65536", i64 romFs = -1, bool measured = false, bool byMakers = false) {
        InstalledGame game = console.describe(idOf(index));
        i64 now = game.totalBytes();
        i64 after = (i64)(now * (1 - ratio));
        std::string answer = generation + format("%016llx|%s|%s\t%lld %lld %lld %lld %lld %d", (unsigned long long)idOf(index), fingerprintOf(game).c_str(), settings, (long long)now,
                                                 (long long)after, (long long)(now / 100), (long long)(romFs < 0 ? now : romFs), (long long)after, compressed ? 1 : 0);
        // 1.4.3 wrote six fields; this version writes whether it was measured, what a copy freed, whether its makers compressed it, whether
        // it's a copy an older version made, and what a packed file takes installed.
        if (generation != std::string("4|"))
            answer += format(" %d 0 %d 0 0 1", measured ? 1 : 0, byMakers ? 1 : 0);
        return answer + "\n";
    }

    /// As writeAnalysis, with Portal Knights Remastered (saving 44%) and Dark Chronicles IV (measured at 2%) compressed by their developer, and
    /// answers for compression off as well: unpacked, Portal Knights would grow by 30%, and the rest come out the same.
    static void writeDeveloperAnalysis(const std::string& path, DemoConsole& console) {
        std::string text;
        auto& list = games();
        for (size_t i = 0; i < list.size(); i++) {
            bool developer = startsWith(list[i].name, "Portal Knights") || startsWith(list[i].name, "Dark Chronicles");
            text += answerFor(console, i, list[i].ratio, false, hbp::app::AnswerGeneration, "9|65536", -1, startsWith(list[i].name, "Dark Chronicles"), developer);
            text += answerFor(console, i, startsWith(list[i].name, "Portal Knights") ? -0.3 : 0.0, false, hbp::app::AnswerGeneration, "0|0", -1, false,
                              startsWith(list[i].name, "Portal Knights"));
        }
        writeText(path, text);
    }

    /// Answers as if every game had already been analyzed, and those given had been found already compressed. Dark Chronicles was measured
    /// in full, as compressing measures a game a first look can't vouch for, so what it saves is known. So was Neon Drift Racing, which would
    /// come out larger: only measured is that a fact about a game, shown in red (a first look's say alone is "not sure", as it may save more).
    static void writeAnalysis(const std::string& path, DemoConsole& console, const std::vector<size_t>& compressed = {}) {
        std::string text;
        auto& list = games();
        for (size_t i = 0; i < list.size(); i++)
            text += answerFor(console, i, list[i].ratio, std::find(compressed.begin(), compressed.end(), i) != compressed.end(), hbp::app::AnswerGeneration, "9|65536", -1,
                              startsWith(list[i].name, "Dark Chronicles") || startsWith(list[i].name, "Neon Drift"));
        writeText(path, text);
    }

    std::vector<u64> listApplications() override {
        std::vector<u64> ids;
        for (size_t i = 0; i < games().size(); i++)
            ids.push_back(idOf(i));
        return ids;
    }

    InstalledGame describe(u64 applicationId) override {
        size_t index = (size_t)((applicationId - 0x0100000000010000ull) >> 16);
        const Game& source = games().at(index);
        InstalledGame game;
        game.applicationId = applicationId;
        game.blocked = source.blocked;
        InstalledMeta base;
        base.key = MetaKey{applicationId, 0, Cnmt::MetaApplication, 0};
        base.storage = index % 5 == 3 ? Storage::Nand : Storage::Sd;
        InstalledContent content;
        content.id.bytes[0] = (u8)index;
        content.size = (i64)(source.gigabytes * (1 << 30));
        base.contents.push_back(content);
        game.metas.push_back(base);
        if (index % 2 == 0) {
            InstalledMeta patch;
            patch.key = MetaKey{applicationId + 0x800, 65536, Cnmt::MetaPatch, 0};
            patch.storage = base.storage;
            game.metas.push_back(patch);
        }
        for (int i = 0; i < source.dlc; i++) {
            InstalledMeta dlc;
            dlc.key = MetaKey{applicationId + 0x1000 + i + 1, 0, Cnmt::MetaAddOnContent, 0};
            dlc.storage = base.storage;
            game.metas.push_back(dlc);
        }
        return game;
    }

    void readControl(InstalledGame& game) override {
        size_t index = (size_t)((game.applicationId - 0x0100000000010000ull) >> 16);
        const Game& source = games().at(index);
        game.name = source.name;
        game.displayVersion = source.version;
        if (source.icon)
            game.icon = {(u8)(60 + index * 37 % 170), (u8)(70 + index * 73 % 150), (u8)(90 + index * 113 % 150), 255};
    }

    SourcePtr openContent(Storage, const ContentId&, i64) override { throw ToolError("This is only a picture of a console"); }
    void addTitleKeys(TicketStore&, const Keyset&) override {}
    i64 freeBytes(Storage storage) override { return storage == Storage::Sd ? 38LL << 30 : 4LL << 30; }
    i64 totalBytes(Storage storage) override { return storage == Storage::Sd ? 238LL << 30 : 26LL << 30; }
    std::unique_ptr<ContentWriter> createContent(Storage, i64) override { throw ToolError("This is only a picture of a console"); }
    void deletePlaceholder(Storage, const std::string&) override {}
    bool hasContent(Storage, const ContentId&) override { return false; }
    void deleteContent(Storage, const ContentId&) override {}
    void setMeta(Storage, const MetaKey&, const Bytes&) override {}
    Bytes getMeta(Storage, const MetaKey&) override { throw ToolError("This is only a picture of a console"); }
    bool hasMeta(Storage, const MetaKey&) override { return false; }
    void removeMeta(Storage, const MetaKey&) override {}
    void setRecords(u64, const std::vector<std::pair<MetaKey, Storage>>&) override {}
    std::vector<std::pair<MetaKey, Storage>> records(u64) override { return {}; }
    void setLaunchVersion(u64, u32) override {}
    std::unique_ptr<FirmwareView> mountGameData(Storage, const ContentId&, u64) override { throw ToolError("This is only a picture of a console"); }
    std::unique_ptr<FirmwareView> mountLogo(Storage, const ContentId&, u64) override { throw ToolError("This is only a picture of a console"); }
};


int runDemo(const std::string& keys, const std::string& folder, const std::string& shots) {
    for (bool dark : {true, false}) {
        std::string data = folder + (dark ? "/demo_dark" : "/demo_light");
        sys::removeTree(data);
        sys::makeDirectories(data + "/data");
        sys::makeDirectories(data + "/sd");
        // Fresh, but for the questions asked once, which the pictures aren't of.
        writeText(data + "/data/settings.txt", "measureall=0\ndeleteasked=1\n");
        DemoConsole console;
        DemoConsole::writeAnalysis(data + "/data/analysis.txt", console);
        HostPlatform platform(data, keys, &console);
        platform.dark = dark;
        App app(platform);
        app.start();
        Person person(app, shots);
        person.looksAtOverview = true;
        std::string theme = dark ? "dark" : "light";
        person.readsWelcome = true;
        person.settle();
        person.shot(theme + "_welcome");
        report("the first time, the welcome screen comes up", app.snapshot().screen == "Welcome", app.snapshot().screen);
        person.press(ButtonDown);
        person.press(ButtonDown);
        person.shot(theme + "_welcome_choice");
        person.readsWelcome = false;
        person.press(ButtonPlus);
        person.waitUntilIdle();
        person.shot(theme + "_overview");
        report("the app opens on the overview", app.snapshot().overview, app.snapshot().headline);
        // The pointer starts on the button that does what's ticked, which says so. A on it asks first. Choose games, beside it, opens the list.
        auto pointedAt = [&] {
            App::Snapshot now = app.snapshot();
            return now.overviewFocus >= 0 && now.overviewFocus < (int)now.overviewStops.size() ? now.overviewStops[(size_t)now.overviewFocus] : App::Snapshot::Stop();
        };
        int toDoCount = 0;
        for (auto& line : app.snapshot().overviewLines)
            toDoCount = line.tab == 0 && line.kind == "ToDo" ? line.count : toDoCount;
        report("the pointer starts on the button that compresses what's ticked", pointedAt().kind == "Act" && pointedAt().label == format("Compress %d games", toDoCount),
               pointedAt().kind + " " + pointedAt().label + format(", %d ticked", toDoCount));
        person.press(ButtonA);
        // Some of the biggest don't fit in what's free, which it says, so it can ask about fewer than the button counts.
        report("A on it asks about compressing those games", startsWith(app.snapshot().dialog, "Compress these"), app.snapshot().dialog);
        person.shot(theme + "_overview_asked");
        person.press(ButtonB);
        person.settle();
        report("and saying no stays on the overview", app.snapshot().overview && app.snapshot().dialog.empty(), app.snapshot().dialog);
        person.press(ButtonRight);
        report("right goes to Choose games beside it", pointedAt().kind == "Choose", pointedAt().kind);
        person.shot(theme + "_overview_choose");
        person.press(ButtonA);
        person.shot(theme + "_list_top");
        report("A on that opens the list it counts", !app.snapshot().overview && app.snapshot().tab == 0);

        // Changing the view keeps the game that has the focus, even going through a view with nothing in it.
        for (int i = 0; i < 4; i++)
            person.press(ButtonDown);
        auto focusedName = [&] {
            App::Snapshot now = app.snapshot();
            return now.focus >= 0 && now.focus < (int)now.rows.size() ? now.rows[(size_t)now.focus].name : std::string("nothing");
        };
        std::string focused = focusedName();
        person.press(ButtonRight);
        person.press(ButtonRight);
        report("changing the view keeps the game that had the focus", app.snapshot().views[0] == "Everything" && focusedName() == focused, app.snapshot().views[0] + ", " + focusedName() + " for " + focused);
        person.shot(theme + "_list_everything");
        person.press(ButtonLeft);
        // Done says what's become of each game: what compressing it freed, or that it's left as it is, without a figure for nothing.
        {
            App::Snapshot done = app.snapshot();
            std::string says;
            bool nothing = false;
            for (auto& row : done.rows) {
                says += row.name + ": " + row.second + " | " + row.saved + "; ";
                nothing = nothing || row.saved.find("Nothing") != std::string::npos || row.second.find("Nothing") != std::string::npos ||
                          row.second.find("not worth") != std::string::npos;
            }
            report("Done says what became of each game, and nothing about what it would save", done.views[0] == "Done" && !done.rows.empty() && !nothing, says);
            person.shot(theme + "_list_done");
        }
        person.press(ButtonLeft);
        report("and coming back too", app.snapshot().views[0] == "To do" && focusedName() == focused, app.snapshot().views[0] + ", " + focusedName() + " for " + focused);
        for (int i = 0; i < 4; i++)
            person.press(ButtonUp);

        for (int i = 0; i < 9; i++)
            person.press(ButtonDown);
        person.shot(theme + "_list_scrolled");
        // The end of the list, where a game that would come out larger goes, and back.
        for (int i = 0; i < 5; i++)
            person.press(ButtonDown);
        person.shot(theme + "_list_end");
        for (int i = 0; i < 5; i++)
            person.press(ButtonUp);

        person.press(ButtonA);
        openSettings(person, app);
        person.shot(theme + "_options");
        person.press(ButtonRight);
        person.press(ButtonRight);
        person.press(ButtonRight);
        person.shot(theme + "_options_changed");
        for (int i = 0; i < 15; i++)
            person.press(ButtonDown);
        person.shot(theme + "_options_end");
        for (int i = 0; i < 15; i++)
            person.press(ButtonUp);
        goToTab(person, app, 0);
        person.settle();
        person.shot(theme + "_list_with_minimum");

        App::Snapshot snapshot = app.snapshot();
        // Measured to save too little, it's settled: under Done, unticked, saying why.
        const App::FullRow* dark = nullptr;
        for (auto& row : snapshot.all)
            dark = row.tab == 0 && containsNoCase(row.name, "Dark Chronicles") ? &row : dark;
        report("the rule leaves out what's measured to save too little, under Done, saying why", dark && !dark->ticked && dark->inDone && startsWith(dark->second, "Not worth it: "),
               dark ? format("ticked %d, done %d, \"%s\"", dark->ticked, dark->inDone, dark->second.c_str()) : "not found");
        // A first look can't say a game saves too little, only that it saves enough: one it can't vouch for isn't ticked until it's measured.
        const App::FullRow* metro = nullptr;
        for (auto& row : snapshot.all)
            metro = startsWith(row.name, "Metro Exodus") ? &row : metro;
        report("one a first look puts below the minimum isn't ticked, and says Thorough analysis measures it", metro && !metro->ticked && metro->notSure &&
               metro->second == "May save more: Thorough analysis measures it", metro ? metro->second : "not found");
        report("the rule ticks what saves enough", rowNamed(snapshot, "Tiny Tunes") && rowNamed(snapshot, "Tiny Tunes")->ticked);

        person.pressTimed(ButtonPlus, theme + "_menu");
        person.shot(theme + "_menu");
        person.press(ButtonRight);
        person.shot(theme + "_sorted_by_size");
        person.press(ButtonB);
        person.press(ButtonY);
        person.shot(theme + "_ticks_changed");
        person.settle(200);

        person.pressTimed(ButtonX, theme + "_compress_dialog");
        person.shot(theme + "_compress_dialog");
        person.press(ButtonB);
        // And the question B asks on the list, the one seen most.
        if (getenv("HBP_TEST_FRAME_TIMES")) {
            person.pressTimed(ButtonB, theme + "_quit_list");
            person.press(ButtonB);
        }

        person.press(ButtonR);
        person.waitUntilIdle();
        person.shot(theme + "_files_empty");
        person.press(ButtonPlus);
        person.shot(theme + "_menu_files");
        person.press(ButtonB);
        // Looking everywhere, Where to look says which folders are left out, and how to look in one of them.
        goToMenuRow(person, app, "What's left out");
        person.press(ButtonA);
        report("Where to look says which folders are left out, and how to look in one", app.snapshot().dialog == "What's left out" &&
               app.snapshot().dialogText.find("Only look in one folder") != std::string::npos, app.snapshot().dialog + ": " + app.snapshot().dialogText);
        person.shot(theme + "_left_out");
        person.press(ButtonB);
        goToMenuRow(person, app, "Only look in one folder");
        person.press(ButtonA);
        person.shot(theme + "_browse");
        person.press(ButtonA);
        person.shot(theme + "_browse_inside");
        person.press(ButtonMinus);
        person.pressTimed(ButtonB, theme + "_quit");
        person.shot(theme + "_quit");
        person.press(ButtonB);
    }

    // Two games already compressed, which are under Done, and at the end of the whole list, after everything still to do. From one of them,
    // left goes to the top of what's to do. The game isn't in it, and nothing to do comes after it on the whole list, which used to put the
    // pointer on the last game.
    {
        std::string data = folder + "/demo_done";
        sys::removeTree(data);
        sys::makeDirectories(data + "/data");
        sys::makeDirectories(data + "/sd");
        writeText(data + "/data/settings.txt", "welcomed=1\nproblemsasked=1\nmeasureall=0\ndeleteasked=1\n");
        DemoConsole console;
        DemoConsole::writeAnalysis(data + "/data/analysis.txt", console, {1, 5});
        HostPlatform platform(data, keys, &console);
        App app(platform);
        app.start();
        Person person(app, shots);
        person.looksAtOverview = true;
        person.waitUntilIdle();
        goToTab(person, app, 0);
        for (int i = 0; i < 3; i++)
            person.press(ButtonDown);
        person.press(ButtonRight);
        App::Snapshot done = app.snapshot();
        person.press(ButtonDown);
        person.press(ButtonLeft);
        App::Snapshot toDo = app.snapshot();
        report("left from a game that's done goes to the top of what's to do, not its last game",
               done.views[0] == "Done" && done.rows.size() == 2 && toDo.views[0] == "To do" && toDo.rows.size() > 1 && toDo.focus == 0,
               format("%s with %d, then %s, on %d of %d", done.views[0].c_str(), (int)done.rows.size(), toDo.views[0].c_str(), toDo.focus, (int)toDo.rows.size()));
        person.shot("todo_from_done");
    }

    // On the overview, the games under a line take the pointer as the lines do. Down from the line goes onto them, A opens their list with
    // the pointer on the game, X does just that game, and a tap puts the pointer on one, a second tap opening it.
    {
        std::string data = folder + "/demo_overview";
        sys::removeTree(data);
        sys::makeDirectories(data + "/data");
        sys::makeDirectories(data + "/sd");
        writeText(data + "/data/settings.txt", "welcomed=1\nproblemsasked=1\nmeasureall=0\ndeleteasked=1\n");
        DemoConsole console;
        DemoConsole::writeAnalysis(data + "/data/analysis.txt", console, {1, 5});
        HostPlatform platform(data, keys, &console);
        App app(platform);
        app.start();
        Person person(app, shots);
        person.looksAtOverview = true;
        person.waitUntilIdle();
        auto pointedAt = [&] {
            App::Snapshot now = app.snapshot();
            return now.overviewFocus >= 0 && now.overviewFocus < (int)now.overviewStops.size() ? now.overviewStops[(size_t)now.overviewFocus] : App::Snapshot::Stop();
        };
        auto lineText = [&](const App::Snapshot::Stop& stop) {
            App::Snapshot now = app.snapshot();
            return stop.line >= 0 && stop.line < (int)now.overviewLines.size() ? now.overviewLines[(size_t)stop.line].text : std::string();
        };
        auto focusedRow = [&] {
            App::Snapshot now = app.snapshot();
            return now.focus >= 0 && now.focus < (int)now.rows.size() ? now.rows[(size_t)now.focus].name : std::string("nothing");
        };
        auto ticks = [&] {
            std::string all;
            for (auto& row : app.snapshot().all)
                all += row.ticked ? "1" : "0";
            return all;
        };
        auto tap = [&](const App::Snapshot::Stop& stop) {
            InputState touch;
            touch.touching = true;
            touch.touchX = stop.x + stop.w / 2;
            touch.touchY = stop.y + stop.h / 2;
            person.frame(touch);
            person.frame();
            person.settle(20);
        };
        std::vector<App::Snapshot::Stop> games;
        for (auto& stop : app.snapshot().overviewStops) {
            if (stop.tab == 0 && !stop.game.empty())
                games.push_back(stop);
        }
        // The ones saving the most, as many as there's room for above the lines at the card's bottom.
        int shownOnCard = 0;
        for (auto& line : app.snapshot().overviewLines)
            shownOnCard = line.tab == 0 && line.kind == "ToDo" ? line.shown : shownOnCard;
        // The cards don't list games: what's to do is said in figures and a bar, and Choose games opens the list.
        (void)shownOnCard;
        report("the cards say what's to do without a list of games", games.empty(), format("%d game stops", (int)games.size()));
        if (games.size() >= 2) {
            person.press(ButtonDown);
            report("down from the line goes onto the first of them", pointedAt().game == games[0].game, pointedAt().game + " for " + games[0].game);
            person.press(ButtonDown);
            report("and down again onto the next", pointedAt().game == games[1].game, pointedAt().game + " for " + games[1].game);
            person.shot("overview_on_game");
            std::string before = ticks();
            person.press(ButtonX);
            App::Snapshot asked = app.snapshot();
            report("X on a game does what the card's button does, as the button shows", startsWith(asked.dialog, "Compress these"), asked.dialog);
            person.press(ButtonB);
            person.settle();
            report("and saying no leaves the ticks as they were, on the overview", ticks() == before && app.snapshot().overview);
            person.press(ButtonA);
            App::Snapshot opened = app.snapshot();
            report("A on a game opens its list on what's to do, with the pointer on it", !opened.overview && opened.tab == 0 && opened.views[0] == "To do" && focusedRow() == games[1].game,
                   opened.views[0] + ", " + focusedRow());
            person.shot("list_from_overview_game");
            person.press(ButtonL);
            report("L goes back to the overview, still on the game", app.snapshot().overview && pointedAt().game == games[1].game, pointedAt().game);
            // The pointer is on the second, so the first is the one to tap.
            const App::Snapshot::Stop other = games[0];
            tap(other);
            report("a tap on a game puts the pointer on it", app.snapshot().overview && pointedAt().game == other.game, pointedAt().game);
            tap(other);
            report("and a second tap opens its list on it", !app.snapshot().overview && focusedRow() == other.game, focusedRow());
            person.press(ButtonL);
            for (int i = 0; i < 6; i++)
                person.press(ButtonDown);
            report("down past the games goes on to what's done, at the bottom of the card, and stops there", pointedAt().game.empty() && lineText(pointedAt()) == "2 done",
                   pointedAt().game + lineText(pointedAt()));
            // Above it, the games a first look can't vouch for, which Thorough analysis measures. A on it says compressing leaves them as they are,
            // and how analysis can measure them by itself.
            person.press(ButtonUp);
            report("above that, a line counts the games that may save more", lineText(pointedAt()) == "1 may save more: Thorough analysis measures it", lineText(pointedAt()));
            person.shot("overview_on_not_sure");
            person.press(ButtonA);
            App::Snapshot said = app.snapshot();
            report("and A on it says compressing leaves it as it is, and how analysis can measure it by itself", said.dialog == "A game may save more" &&
                   said.dialogText.find("compressing leaves it as it is") != std::string::npos && said.dialogText.find("Analysis, in Settings") != std::string::npos,
                   said.dialog + ": " + said.dialogText);
            person.shot("overview_measure_said");
            person.press(ButtonB);
            person.settle();
        }
    }

    // What 1.4.3 worked out, as it wrote it. Since then, a piece of the original game that an update holds more than once is stored once, and
    // game data under 1 MB is left as it is, so its answers for a game with an update or DLC, with that little game data, or with little or nothing
    // to gain, are worked out again. The rest are still good, as is anything worked out with compression off, and whether a game was rebuilt. The
    // pretend console can't be read, so a game that's worked out again can't be analyzed, which doesn't matter here.
    {
        std::string data = folder + "/demo_previous";
        sys::removeTree(data);
        sys::makeDirectories(data + "/data");
        sys::makeDirectories(data + "/sd");
        writeText(data + "/data/settings.txt", "welcomed=1\nproblemsasked=1\nmeasureall=0\ndeleteasked=1\n");
        DemoConsole console;
        auto& games = DemoConsole::games();
        std::string text;
        // Tiny Tunes has under 1 MB of game data, and Animal Gardens, with its DLC, saves plenty. Celestial Peak was already compressed, which
        // is looked at again, for what it freed and which version made it.
        for (size_t i = 0; i < games.size(); i++)
            text += DemoConsole::answerFor(console, i, i == 11 ? 0.3 : games[i].ratio, i == 5, "4|", "9|65536", i == 9 ? 900000 : -1);
        std::string offAnswer = DemoConsole::answerFor(console, 3, 0.01, false, "4|", "0|0");
        std::string rebuilt = DemoConsole::fingerprintOf(console.describe(DemoConsole::idOf(0)));
        std::string offKey = "5" + offAnswer.substr(1, offAnswer.find('\t'));
        // The first time with nothing else remembered, and then with the list as it was, whose games are only known to have an update or DLC
        // once they've been looked at again.
        for (const char* when : {"with nothing else remembered", "with the list remembered from last time"}) {
            if (std::string(when).find("last time") != std::string::npos)
                report("the list from the first time is there to start from", sys::exists(data + "/data/list.cache"));
            for (const char* where : {"/data/analysis.txt", "/config/analysis.txt"})
                sys::removeFile(data + where);
            writeText(data + "/data/analysis.txt", text + offAnswer + "4|rebuilt|" + rebuilt + "\t0\n");
            {
                HostPlatform platform(data, keys, &console);
                App app(platform);
                app.start();
                Person person(app, shots);
                person.looksAtOverview = true;
                person.waitUntilIdle();
                App::Snapshot s = app.snapshot();
                std::string kept, again, wrong;
                for (size_t i = 0; i < games.size(); i++) {
                    if (*games[i].blocked)
                        continue;
                    const App::FullRow* row = nullptr;
                    for (auto& r : s.all) {
                        if (r.tab == 0 && r.key == format("%016llx", (unsigned long long)DemoConsole::idOf(i)))
                            row = &r;
                    }
                    bool stillGood = i == 1 || i == 3 || i == 7;
                    bool recalled = row && row->hasEstimate;
                    (recalled ? kept : again) += std::string(games[i].name).substr(0, 14) + "; ";
                    if (!row || recalled != stillGood)
                        wrong += format("%s %s; ", games[i].name, recalled ? "was kept" : "wasn't kept");
                }
                report(std::string("answers 1.4.3 worked out are kept where nothing since changes them, and worked out again where something could, ") + when, wrong.empty(),
                       wrong + "| kept: " + kept + "| again: " + again);
                person.shot("previous_answers");
            }
            std::string saved = readText(data + "/data/analysis.txt");
            report(std::string("what's kept is saved as this version's, compression off and rebuilt too, and none is left as the version before's, ") + when,
                   saved.rfind("4|", 0) != 0 && saved.find("\n4|") == std::string::npos && saved.find(offKey) != std::string::npos && saved.find("5|rebuilt|" + rebuilt + "\t0\n") != std::string::npos,
                   saved);
        }
    }

    // No keys yet.
    {
        std::string data = folder + "/demo_nokeys";
        sys::removeTree(data);
        sys::makeDirectories(data);
        DemoConsole console;
        HostPlatform platform(data, data + "/prod.keys", &console);
        App app(platform);
        app.start();
        Person person(app, shots);
        person.settle();
        person.shot("nokeys");
        report("no keys, and it says so", app.snapshot().screen == "NoKeys");
    }

    // The console's services can't be had.
    {
        std::string data = folder + "/demo_noconsole";
        sys::removeTree(data);
        sys::makeDirectories(data + "/sd");
        HostPlatform platform(data, keys, nullptr);
        App app(platform);
        app.start();
        Person person(app, shots);
        person.looksAtOverview = true;
        person.waitUntilIdle();
        person.shot("noconsole_overview");
        goToTab(person, app, 1);
        person.shot("noconsole_files");
        person.press(ButtonL);
        person.shot("noconsole_installed");
    }

    // A game its developer compressed, worth compressing again (Portal Knights Remastered), is to do, but never ticked by itself, as its
    // updates then need its original: its line, the overview and the question say so, compressing or repacking. One that isn't worth it
    // (Dark Chronicles IV, measured) is under Done, as any other game is.
    for (bool compress : {true, false}) {
        std::string data = folder + (compress ? "/demo_developer" : "/demo_developer_repack");
        sys::removeTree(data);
        sys::makeDirectories(data + "/data");
        sys::makeDirectories(data + "/sd");
        writeText(data + "/data/settings.txt", std::string("welcomed=1\nproblemsasked=1\nmeasureall=0\ndeleteasked=1\n") + (compress ? "" : "compress=0\n"));
        DemoConsole console;
        DemoConsole::writeDeveloperAnalysis(data + "/data/analysis.txt", console);
        HostPlatform platform(data, keys, &console);
        App app(platform);
        app.start();
        Person person(app, shots);
        person.looksAtOverview = true;
        person.waitUntilIdle();
        const std::string developer = "Portal Knights Remastered", notWorth = "Dark Chronicles IV";
        auto rowOf = [&](const std::string& name) {
            App::FullRow found;
            for (auto& row : app.snapshot().all) {
                if (row.tab == 0 && row.name == name)
                    found = row;
            }
            return found;
        };
        if (compress) {
            std::string line;
            for (auto& each : app.snapshot().overviewLines) {
                if (each.tab == 0 && each.kind == "Developer")
                    line = each.text;
            }
            person.shot("developer_overview");
            report("the overview has a line for the game its developer compressed, which isn't ticked", line == "1 compressed by its developer, not ticked", line);
            App::FullRow row = rowOf(developer);
            report("worth compressing again, it's to do, unticked, and its line says its updates then need its original",
                   row.inToDo && !row.ticked && row.second == "Compressed by its developer: updating it then needs its original",
                   format("to do %d, ticked %d: ", (int)row.inToDo, (int)row.ticked) + row.second);
            App::FullRow low = rowOf(notWorth);
            report("one its developer compressed that isn't worth it is under Done, saying so as any other game does",
                   low.inDone && !low.ticked && startsWith(low.second, "Not worth it"), format("done %d: ", (int)low.inDone) + low.second);
            // Its line opens the list on that game, whose own line says why it isn't ticked, rather than at the top of To do.
            App::Snapshot home = app.snapshot();
            for (auto& stop : home.overviewStops) {
                if (stop.tab != 0 || stop.line < 0 || stop.line >= (int)home.overviewLines.size() || home.overviewLines[(size_t)stop.line].kind != "Developer")
                    continue;
                InputState touch;
                touch.touching = true;
                touch.touchX = stop.x + stop.w / 2;
                touch.touchY = stop.y + stop.h / 2;
                person.frame(touch);
                person.frame();
                person.settle(20);
                // A tap puts the pointer on it, and A opens it.
                if (app.snapshot().overview)
                    person.press(ButtonA);
                break;
            }
            App::Snapshot opened = app.snapshot();
            std::string onRow = opened.focus >= 0 && opened.focus < (int)opened.rows.size() ? opened.rows[(size_t)opened.focus].name : std::string("nothing");
            report("its line opens the list on that game", opened.screen == "Main" && opened.tab == 0 && onRow == developer, opened.screen + ", " + onRow);
        }
        // Ticked by hand, X asks first, saying what it means for its updates.
        goToTab(person, app, 0);
        for (int i = 0; i < 3 && app.snapshot().views[0] != "Everything"; i++)
            person.press(ButtonRight);
        int at = -1;
        App::Snapshot listed = app.snapshot();
        for (int i = 0; i < (int)listed.rows.size(); i++) {
            if (listed.rows[(size_t)i].name == developer)
                at = i;
        }
        for (int i = 0; i < 40 && at >= 0 && app.snapshot().focus != at; i++)
            person.press(app.snapshot().focus < at ? ButtonDown : ButtonUp);
        if (!rowOf(developer).ticked)
            person.press(ButtonA);
        bool tickedByHand = rowOf(developer).ticked;
        person.press(ButtonX);
        App::Snapshot asked = app.snapshot();
        std::string said = developer + (compress ? " was compressed by its developer. Once it's compressed again here, updating it needs its original game file."
                                                 : " was compressed by its developer. Once it's repacked here, updating it needs its original game file.");
        person.shot(compress ? "developer_asked" : "developer_asked_repack");
        report(std::string(compress ? "compressing" : "repacking") + " it, ticked by hand, the question says its updates then need its original",
               at >= 0 && tickedByHand && asked.dialogText.find(said) != std::string::npos, asked.dialog + ": " + asked.dialogText);
        person.press(ButtonB);
        person.settle();
    }
    printf("%d passed, %d failed\n", g_passes, g_failures);
    return g_failures > 0 ? 1 : 0;
}

/// A look at what text comes to with fonts of every kind of outline. The console's own fonts can't be had here.
int runFonts(const std::string& shots) {
    std::vector<u32> pixels(1280 * 720);
    Canvas canvas(pixels.data(), 1280, 720, 1280);
    canvas.clear(Color{27, 29, 32});
    static const char* const paths[] = {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/Crimson-Roman.otf", "C:/Windows/Fonts/NotoSansJP-Regular.otf"};
    float y = 40;
    bool allRead = true;
    for (const char* path : paths) {
        Bytes data = readWhole(path);
        Font font;
        font.addFace(data.data(), data.size());
        allRead = allRead && font.hasFaces();
        Color text{238, 240, 242};
        canvas.text(font, 40, y, path, 15, Color{163, 170, 178});
        y += 34;
        canvas.text(font, 40, y, "The Legend of the Hollow Crown: Tears of Time 0123456789", 27, text);
        y += 30;
        canvas.text(font, 40, y, "Version 1.2.1 with its update  \xC2\xB7  2 DLC  \xC2\xB7  On the SD card  (2.20 GB, 51%)", 15, text);
        y += 30;
        canvas.text(font, 40, y, "Compressing the game data", 20, text, true);
        y += 34;
        canvas.text(font, 40, y, "\xE3\x82\xBC\xE3\x83\xAB\xE3\x83\x80\xE3\x81\xAE\xE4\xBC\x9D\xE8\xAA\xAC \xE3\x81\x82\xE3\x81\x84\xE3\x81\x86 abc", 24, text);
        y += 72;
    }
    report("fonts of every kind can be read", allRead);
    writePng(shots + "/fonts.png", pixels.data(), 1280, 720);
    return g_failures > 0 ? 1 : 0;
}

/// Draws the icon the app shows on the home screen of the homebrew menu, and saves it as a JPEG.
int writeIcon(const std::string& path) {
    constexpr int Size = 256;
    std::vector<u32> pixels(Size * Size);
    Canvas c(pixels.data(), Size, Size, Size);
    c.clear(Color{27, 29, 32});
    const Color teal{45, 190, 172};
    const Color pale{226, 244, 241};

    // A tall stack of data, squeezed down to a short one.
    int width[] = {150, 150, 150};
    for (int i = 0; i < 3; i++)
        c.fillRound(Rect{(Size - width[i]) / 2, 44 + i * 24, width[i], 16}, 5, pale.withAlpha(90 + i * 30));
    c.line(128, 120, 128, 150, 9, teal);
    c.line(128, 158, 106, 134, 9, teal);
    c.line(128, 158, 150, 134, 9, teal);
    c.fillRound(Rect{53, 172, 150, 30}, 9, teal);

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return 1;
    std::wstring wide(path.begin(), path.end());
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* properties = nullptr;
    bool ok = SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(wide.c_str(), GENERIC_WRITE)) && SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) && SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, &properties));
    if (ok) {
        PROPBAG2 option = {};
        wchar_t name[] = L"ImageQuality";
        option.pstrName = name;
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = 0.95f;
        properties->Write(1, &option, &value);
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        Bytes bgr((size_t)Size * Size * 3);
        for (int i = 0; i < Size * Size; i++) {
            u32 p = pixels[(size_t)i];
            bgr[(size_t)i * 3] = (u8)(p >> 16);
            bgr[(size_t)i * 3 + 1] = (u8)(p >> 8);
            bgr[(size_t)i * 3 + 2] = (u8)p;
        }
        ok = SUCCEEDED(frame->Initialize(properties)) && SUCCEEDED(frame->SetSize(Size, Size)) && SUCCEEDED(frame->SetPixelFormat(&format)) && SUCCEEDED(frame->WritePixels(Size, Size * 3, (UINT)bgr.size(), bgr.data())) && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }
    if (properties)
        properties->Release();
    if (frame)
        frame->Release();
    if (encoder)
        encoder->Release();
    if (stream)
        stream->Release();
    factory->Release();
    printf(ok ? "wrote %s\n" : "couldn't write %s\n", path.c_str());
    return ok ? 0 : 1;
}

/// How much memory the hash tree of a very large game takes, which only has to be there once. A game of 160 GB comes to a good 300 MB of it.
int runHashTree(int gigabytes) {
    i64 bytes = (i64)gigabytes << 30;
    Bytes data(1 << 20);
    PROCESS_MEMORY_COUNTERS before = {};
    GetProcessMemoryInfo(GetCurrentProcess(), &before, sizeof(before));
    IvfcBuilder builder;
    builder.expect(bytes);
    for (i64 done = 0; done < bytes; done += (i64)data.size())
        builder.add(data.data(), data.size());
    IvfcResult result = builder.finish(bytes, true);
    PROCESS_MEMORY_COUNTERS after = {};
    GetProcessMemoryInfo(GetCurrentProcess(), &after, sizeof(after));
    i64 levels = (i64)result.hashLevels.size();
    i64 peak = (i64)after.PeakPagefileUsage - (i64)before.PagefileUsage;
    printf("  %d GB of data: %lld MB of hash levels, %lld MB at the most\n", gigabytes, (long long)(levels >> 20), (long long)(peak >> 20));
    report("the size of the hash levels is what it should be", levels == IvfcBuilder::hashLevelsSize(bytes));
    report("and they only had to be in memory once", peak < levels + levels / 8, format("%lld MB", (long long)(peak >> 20)));
    return g_failures > 0 ? 1 : 0;
}

/// Everything from start to finish, with real games, on the pretend console.
int runReal(const std::string& keys, const std::string& folder, const std::string& shots, const std::vector<std::string>& nsps) {
    Keyset keyset;
    keyset.load(keys);

    std::string root = folder + "/real";
    sys::removeTree(root);
    sys::makeDirectories(root + "/sd/games");

    // What a keys file without the keys each master key leads to has worked out for it is the same as what Lockpick would have written.
    // Only compared, never shown.
    {
        std::string kept;
        int left = 0;
        std::string text = readText(keys);
        for (size_t start = 0; start < text.size();) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos)
                end = text.size();
            std::string line = text.substr(start, end - start);
            start = end + 1;
            std::string name = line.substr(0, line.find('='));
            bool derived = (name.rfind("key_area_key_", 0) == 0 || name.rfind("titlekek_", 0) == 0) && name.find("source") == std::string::npos;
            if (derived)
                left++;
            else
                kept += line + "\n";
        }
        sys::makeDirectories(root + "/fewer_keys");
        writeText(root + "/fewer_keys/prod.keys", kept);
        Keyset fewer;
        fewer.load(root + "/fewer_keys/prod.keys");
        int compared = 0, same = 0;
        for (int generation = 0; generation < 0x40; generation++) {
            for (int kind = 0; kind < 3; kind++) {
                const u8* full = keyset.keyAreaKey(kind, generation);
                if (!full)
                    continue;
                compared++;
                const u8* worked = fewer.keyAreaKey(kind, generation);
                same += worked && memcmp(full, worked, 16) == 0 ? 1 : 0;
            }
            if (const u8* full = keyset.titleKek(generation)) {
                compared++;
                const u8* worked = fewer.titleKek(generation);
                same += worked && memcmp(full, worked, 16) == 0 ? 1 : 0;
            }
        }
        report("keys left out of the file are worked out from the master keys, the same as Lockpick's", left > 0 && compared > 0 && same == compared, format("%d of %d, %d lines left out", same, compared, left));
        sys::removeTree(root + "/fewer_keys");
    }
    std::string consoleFolder = root + "/console";

    auto appearance = [&](SimConsole& console) {
        for (auto& path : nsps) {
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(path));
            set.group();
            for (auto& job : set.jobs) {
                u64 application = job.isBaseGame() ? job.main->meta.titleId : job.main->meta.linkedId();
                if (job.isBaseGame() && !job.titleName.empty())
                    console.setAppearance(application, job.titleName, readTitleIcon(job));
            }
        }
    };

    auto freshConsole = [&] {
        sys::removeTree(consoleFolder);
        wipeData(root);
        auto console = std::make_unique<SimConsole>(consoleFolder, keyset);
        console->install(nsps, Storage::Sd);
        appearance(*console);
        return console;
    };

    auto healthy = [&](SimConsole& console, const std::string& when) {
        std::string problem = console.checkConsistent(keyset);
        report(when + ": every game is whole", problem.empty(), problem);
        auto stray = console.strayFiles();
        report(when + ": nothing left lying around", stray.empty(), stray.empty() ? "" : stray[0]);
        report(when + ": no journal left", !sys::exists(root + "/data/journal/journal.txt"));
    };

    auto pcInstall = [&] {
        const char* share = getenv("HBP_TEST_PC_SHARE");
        if (getenv("HBP_TEST_PC") && *getenv("HBP_TEST_PC") && share && *share) {
            wipeData(root);
            const std::string& alone = nsps.size() > 2 ? nsps[2] : nsps[0];
            // The game, and its update and DLC when there are some, which the list groups under it.
            std::vector<std::pair<std::string, std::string>> shared{{alone, "from the pc.nsp"}};
            if (nsps.size() > 4) {
                shared.emplace_back(nsps[3], "from the pc update.nsp");
                shared.emplace_back(nsps[4], "from the pc dlc.nsp");
            }
            for (auto& file : shared) {
                Bytes bytes = readWhole(file.first);
                sys::File out(std::string(share) + "/" + file.second, sys::File::Mode::Create);
                out.write(0, bytes.data(), bytes.size());
            }
            // Nothing else on the list, and the settings as they are to begin with: PCs are looked in, and game files get installed.
            sys::removeTree(root + "/sd/games");
            sys::makeDirectories(root + "/sd/games");
            writeText(root + "/data/settings.txt", "welcomed=1\nproblemsasked=1\nmeasureall=0\ndeleteasked=1\nwritelog=1\n");
            SimConsole console(root + "/console8", keyset);
            HostPlatform platform(root, keys, &console);
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToTab(person, app, 1);
            person.waitUntilIdle();
            person.settle();
            std::vector<int> fromPc;
            std::string listed;
            auto rows = app.snapshot().rows;
            for (int i = 0; i < (int)rows.size(); i++) {
                listed += rows[(size_t)i].name + " / " + rows[(size_t)i].detail + "; ";
                if (rows[(size_t)i].detail.find("On Test-PC") != std::string::npos)
                    fromPc.push_back(i);
            }
            report("what the PC shares is on the list of game files, as one game", fromPc.size() == 1, listed);
            person.shot("real_files_from_pc");
            std::string log = readText(root + "/data/log.txt");
            // An older PC can't say, and then the console reads the files itself, which the tests run both ways.
            printf("  The PC %s the game\n", log.find("Analyzed on the PC") != std::string::npos ? "analyzed" : "didn't analyze");
            report("and it's been analyzed", !fromPc.empty() && rows[(size_t)fromPc[0]].hasEstimate, listed);
            if (!fromPc.empty()) {
                for (int step = 0; step < fromPc[0]; step++)
                    person.press(ButtonDown);
            }
            chooseInMenu(person, app, "Install this game");
            bool asked = person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 120);
            report("installing from the PC asks first", asked, app.snapshot().dialog);
            double started = sys::now();
            person.press(ButtonA);
            person.waitUntilIdle();
            report("and it installs over the network", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
            printf("  Installing took %.1f s\n", sys::now() - started);
            std::string problem = console.checkConsistent(keyset);
            std::string what = problem;
            for (auto& line : app.snapshot().results)
                what += "; " + line;
            report("whole", problem.empty() && console.listApplications().size() == 1, what);
            person.press(ButtonA);
            person.waitUntilIdle();
            person.settle();
            bool stillThere = false;
            for (auto& row : app.snapshot().rows)
                stillThere = stillThere || row.detail.find("On Test-PC") != std::string::npos;
            report("once it's installed compressed, there's nothing left to do with it, so it's off the list", !stillThere, format("%d hidden", app.snapshot().compressedHidden));

            // The same PC on a cable as well, which the host build reaches over the network underneath.
            _putenv_s("HBP_TEST_PC_CABLE", "1");
            auto found = remote::peers();
            report("a PC on both the cable and the network is only looked in on the cable", found.size() == 1 && found[0].id == "USB",
                found.empty() ? "none" : found[0].id + format(" and %d more", (int)found.size() - 1));
            // What it shares is in a folder named after the one shared.
            std::string onCable = "pc:USB/";
            for (auto& entry : remote::list(onCable)) {
                if (entry.isDirectory) {
                    onCable += entry.name + "/" + shared[0].second;
                    break;
                }
            }
            {
                remote::RemoteFile file(onCable, remote::fileSize(onCable));
                u8 first[4] = {};
                file.read(0, first, sizeof(first));
                report("and its games are read through the cable", memcmp(first, "PFS0", 4) == 0);
            }
            // All of a file, from front to back as installing reads it: through the cable the next piece is asked for while one is worked on,
            // and with Wi-Fi first a PC on both is read over the network. Either way it's what the PC has, and the log says how it came.
            struct Said : JobSink {
                sys::Mutex mutex;
                std::string text;
                void message(const std::string& line) override {
                    sys::Lock lock(mutex);
                    text += line + "\n";
                }
            } said;
            auto readAll = [&](const std::string& path) {
                setSink(&said);
                Bytes got;
                {
                    remote::RemoteFile whole(path, remote::fileSize(path));
                    got.resize((size_t)std::max<i64>(whole.size(), 0));
                    constexpr i64 Chunk = 1 << 20;
                    for (i64 at = 0; at < whole.size(); at += Chunk)
                        whole.read(at, got.data() + at, (size_t)std::min<i64>(Chunk, whole.size() - at));
                }
                setSink(nullptr);
                return got;
            };
            Bytes original = readWhole(alone);
            report("all of a game read through the cable, a piece ahead, is what the PC has", readAll(onCable) == original, format("%lld bytes", (long long)original.size()));
            report("and the log says it came through the cable, and how fast", original.size() < (16u << 20) || said.text.find("from the PC on the USB cable") != std::string::npos, said.text);
            remote::setWifiFirst(true);
            remote::forgetLinks();
            auto wifi = remote::peers();
            report("with Wi-Fi first, a PC on both is only looked in over the network", wifi.size() == 1 && wifi[0].id == "Test-PC",
                wifi.empty() ? "none" : wifi[0].id + format(" and %d more", (int)wifi.size() - 1));
            std::string overNetwork = "pc:Test-PC/" + onCable.substr(std::string("pc:USB/").size());
            said.text.clear();
            report("and all of it read over the network is what the PC has", readAll(overNetwork) == original);
            report("and the log says it came over the network", original.size() < (16u << 20) || said.text.find("Test-PC, over the network") != std::string::npos, said.text);
            remote::setWifiFirst(false);
            remote::forgetLinks();
            auto withoutCable = remote::peers(false);
            report("with the cable left out of where to look, it's on the network again", withoutCable.size() == 1 && withoutCable[0].id == "Test-PC");
            _putenv_s("HBP_TEST_PC_CABLE", "");

            // The same game on the SD card as well, on a console without it: that's the copy used, and the PC's is noted on it.
            {
                Bytes bytes = readWhole(alone);
                sys::File out(root + "/sd/games/on the sd card.nsp", sys::File::Mode::Create);
                out.write(0, bytes.data(), bytes.size());
            }
            sys::removeTree(root + "/console9");
            SimConsole other(root + "/console9", keyset);
            HostPlatform otherPlatform(root, keys, &other);
            App both(otherPlatform);
            both.start();
            Person looking(both, shots);
            looking.waitUntilIdle();
            goToTab(looking, both, 1);
            looking.waitUntilIdle();
            looking.settle();
            std::string seen;
            int onSd = 0, onPc = 0, noted = 0;
            for (auto& row : both.snapshot().rows) {
                seen += row.name + " / " + row.detail + format(" (%d notes); ", row.notes);
                onSd += row.detail.find("On the SD card") != std::string::npos;
                onPc += row.detail.find("On Test-PC") != std::string::npos;
                noted += row.notes;
            }
            report("a game on the SD card and on a PC is listed once, from the SD card, with the other copy noted", onSd == 1 && onPc == 0 && noted > 0, seen);
            sys::removeFile(root + "/sd/games/on the sd card.nsp");
            for (auto& file : shared)
                sys::removeFile(std::string(share) + "/" + file.second);
        }
    };

    // Which way through to a PC wins, when there's more than one.
    {
        auto peer = [](const char* id, const char* machine) { return remote::Peer{id, id, nullptr, machine}; };
        std::vector<remote::Peer> both{peer("Desk", "Desk"), peer("Laptop", "Laptop"), peer("USB", "Desk")};
        remote::keepOneWayEach(both);
        report("a PC on the cable isn't listed over the network as well, while another PC is",
            both.size() == 2 && both[0].id == "Laptop" && both[1].id == "USB");
        std::vector<remote::Peer> unnamed{peer("Desk", "Desk"), peer("USB", "")};
        remote::keepOneWayEach(unnamed);
        report("a PC on the cable that hasn't said its name leaves the network ones alone", unnamed.size() == 2);
        remote::setWifiFirst(true);
        std::vector<remote::Peer> wifi{peer("Desk", "Desk"), peer("Laptop", "Laptop"), peer("USB", "Desk")};
        remote::keepOneWayEach(wifi);
        report("with Wi-Fi first, a PC on both is only listed over the network", wifi.size() == 2 && wifi[0].id == "Desk" && wifi[1].id == "Laptop");
        std::vector<remote::Peer> cableOnly{peer("Laptop", "Laptop"), peer("USB", "Desk")};
        remote::keepOneWayEach(cableOnly);
        report("and a PC only on the cable is still listed", cableOnly.size() == 2 && cableOnly[1].id == "USB");
        remote::setWifiFirst(false);
    }

    // Pictures inside games, as Look inside shows them: PNGs with each of the five ways a row can be stored and the colour types games use,
    // a BMP, and the size of a JPEG, which the console's own decoder needs told.
    {
        auto png = [](u32 width, u32 height, int depth, int colour, const Bytes& rows, const Bytes& palette) {
            Bytes out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
            auto chunk = [&](const char* type, const Bytes& body) {
                u32 n = (u32)body.size();
                Bytes head = {(u8)(n >> 24), (u8)(n >> 16), (u8)(n >> 8), (u8)n, (u8)type[0], (u8)type[1], (u8)type[2], (u8)type[3]};
                out.insert(out.end(), head.begin(), head.end());
                out.insert(out.end(), body.begin(), body.end());
                out.insert(out.end(), 4, 0);
            };
            chunk("IHDR", Bytes{(u8)(width >> 24), (u8)(width >> 16), (u8)(width >> 8), (u8)width, (u8)(height >> 24), (u8)(height >> 16), (u8)(height >> 8),
                                (u8)height, (u8)depth, (u8)colour, 0, 0, 0});
            if (!palette.empty())
                chunk("PLTE", palette);
            // A zlib stream holding the rows as one stored Deflate block.
            size_t n = rows.size();
            Bytes z = {0x78, 0x01, 0x01, (u8)n, (u8)(n >> 8), (u8)~n, (u8)(~n >> 8)};
            z.insert(z.end(), rows.begin(), rows.end());
            z.insert(z.end(), 4, 0);
            chunk("IDAT", z);
            chunk("IEND", Bytes());
            return out;
        };
        auto paeth = [](int a, int b, int c) {
            int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
            return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
        };
        // Five rows of three RGB pixels, each row stored a different way.
        constexpr int W = 3, H = 5;
        auto value = [](int x, int y, int channel) { return (u8)((x * 53 + y * 97 + channel * 71 + x * y * 13) & 255); };
        Bytes rows;
        for (int y = 0; y < H; y++) {
            int filter = y;
            rows.push_back((u8)filter);
            for (int x = 0; x < W * 3; x++) {
                int now = value(x / 3, y, x % 3);
                int a = x >= 3 ? value((x - 3) / 3, y, x % 3) : 0, b = y > 0 ? value(x / 3, y - 1, x % 3) : 0;
                int c = x >= 3 && y > 0 ? value((x - 3) / 3, y - 1, x % 3) : 0;
                int predicted = filter == 1 ? a : filter == 2 ? b : filter == 3 ? (a + b) / 2 : filter == 4 ? paeth(a, b, c) : 0;
                rows.push_back((u8)(now - predicted));
            }
        }
        Image image;
        bool decoded = decodePng(png(W, H, 8, 2, rows, Bytes()), image);
        bool right = decoded && image.w == W && image.h == H;
        for (int y = 0; right && y < H; y++) {
            for (int x = 0; x < W; x++) {
                u32 p = image.pixels[(size_t)y * W + x];
                right = right && (p & 255) == value(x, y, 0) && (p >> 8 & 255) == value(x, y, 1) && (p >> 16 & 255) == value(x, y, 2) && (p >> 24) == 255;
            }
        }
        report("a PNG is decoded, whichever way each of its rows is stored", right);
        // Two colours in a palette, a bit a pixel, and see-through grey.
        Image palette;
        bool paletted = decodePng(png(8, 1, 1, 3, Bytes{0, 0xA5}, Bytes{255, 0, 0, 0, 0, 255}), palette);
        report("and one with a palette, eight pixels to a byte", paletted && palette.w == 8 && palette.pixels[0] == 0xFFFF0000u && palette.pixels[1] == 0xFF0000FFu,
               paletted ? format("%08X %08X", palette.pixels[0], palette.pixels[1]) : std::string("not decoded"));
        Image grey;
        bool greyed = decodePng(png(1, 1, 8, 4, Bytes{0, 200, 100}, Bytes()), grey);
        report("and grey that's partly see-through", greyed && grey.pixels[0] == (200u | 200u << 8 | 200u << 16 | 100u << 24));
        Bytes broken = png(W, H, 8, 2, rows, Bytes());
        broken.resize(broken.size() - 30);
        Image none;
        report("a PNG cut short isn't taken for a picture", !decodePng(broken, none));

        // A BMP of two by two, kept bottom row first, each row padded to four bytes.
        Bytes bmp(54 + 16, 0);
        bmp[0] = 'B';
        bmp[1] = 'M';
        bmp[10] = 54;
        bmp[14] = 40;
        bmp[18] = 2;
        bmp[22] = 2;
        bmp[26] = 1;
        bmp[28] = 24;
        // Bottom row: blue, white. Top row: red, green. Each as blue, green, red.
        const u8 pixels[] = {255, 0, 0, 255, 255, 255, 0, 0, 0, 0, 255, 0, 255, 0, 0, 0};
        memcpy(bmp.data() + 54, pixels, sizeof(pixels));
        Image fromBmp;
        bool bmpRead = decodeBmp(bmp, fromBmp);
        report("a BMP is decoded the right way up", bmpRead && fromBmp.pixels[0] == 0xFF0000FFu && fromBmp.pixels[1] == 0xFF00FF00u && fromBmp.pixels[2] == 0xFFFF0000u,
               bmpRead ? format("%08X %08X %08X", fromBmp.pixels[0], fromBmp.pixels[1], fromBmp.pixels[2]) : std::string("not decoded"));
        Bytes jpeg = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x00, 0x00, 0xFF, 0xC0, 0x00, 0x11, 0x08, 0x01, 0x2C, 0x02, 0x80, 0x03, 0, 0, 0, 0, 0, 0};
        int jw = 0, jh = 0;
        report("and a JPEG's size is found", jpegSize(jpeg, jw, jh) && jw == 640 && jh == 300, format("%d x %d", jw, jh));
    }

    // While the list of game files waits on a PC that's stopped answering, what's asked for still goes first.
    auto waitingOnPc = [&] {
        auto console = freshConsole();
        sys::removeTree(root + "/sd/games");
        // Whatever an earlier test left the settings at, PCs on the network are looked in, and afterwards the settings are as they were,
        // as what comes next looks through its lists before it's analyzing.
        std::vector<std::pair<std::string, std::string>> settingsWere;
        for (const std::string& path : {root + "/data/settings.txt", root + "/config/settings.txt"}) {
            if (!sys::exists(path))
                continue;
            std::string text = readText(path);
            settingsWere.push_back({path, text});
            size_t at = text.find("frompcnetwork=0");
            if (at != std::string::npos)
                writeText(path, text.replace(at, 15, "frompcnetwork=1"));
        }
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            std::string before = getenv("HBP_TEST_PC") ? getenv("HBP_TEST_PC") : "";
            {
                SilentPc silent(25890);
                _putenv_s("HBP_TEST_PC", "127.0.0.1:25890");
                _putenv_s("HBP_TEST_PC_NAME", "Silent-PC");
                goToTab(person, app, 1);
                person.waitUntilIdle();
                chooseInMenu(person, app, "Refresh the list");
                person.waitFor([&] { return app.snapshot().scanning; }, 10);
                sys::sleepMs(1000);
                goToTab(person, app, 0);
                report("with games ticked and game files waiting on a PC", app.snapshot().scanning && app.snapshot().dialog.empty(),
                    format("%d connections to it, %s", silent.connections(), app.snapshot().dialog.c_str()));
                double started = sys::now();
                person.press(ButtonX);
                person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 60);
                double waited = sys::now() - started;
                report("compressing asks straight away all the same", app.snapshot().dialog.find("Compress") == 0 && waited < 5, format("%s after %.1f s", app.snapshot().dialog.c_str(), waited));
                person.press(ButtonB);
                _putenv_s("HBP_TEST_PC", before.c_str());
                _putenv_s("HBP_TEST_PC_NAME", "");
            }
            person.waitUntilIdle();
            {
                // A PC that had stopped sharing, which doesn't even answer the connection: connecting to it has to give way too. Waiting on
                // one kept a real console's app from answering for minutes. Nothing ever answers at this address, as it's kept for examples.
                _putenv_s("HBP_TEST_PC", "192.0.2.1:25891");
                _putenv_s("HBP_TEST_PC_NAME", "Gone-PC");
                goToTab(person, app, 1);
                person.waitUntilIdle();
                chooseInMenu(person, app, "Refresh the list");
                person.waitFor([&] { return app.snapshot().scanning; }, 10);
                sys::sleepMs(1000);
                goToTab(person, app, 0);
                double started = sys::now();
                person.press(ButtonX);
                person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 60);
                double waited = sys::now() - started;
                report("with the list connecting to a PC that never answers, compressing asks straight away", app.snapshot().dialog.find("Compress") == 0 && waited < 5,
                    format("%s after %.1f s", app.snapshot().dialog.c_str(), waited));
                person.press(ButtonB);
                person.waitUntilIdle();
                report("and connecting to it gives up by itself", !app.snapshot().scanning);
                _putenv_s("HBP_TEST_PC", before.c_str());
                _putenv_s("HBP_TEST_PC_NAME", "");
            }
            person.waitUntilIdle();
        }
        for (auto& [path, text] : settingsWere)
            writeText(path, text);
    };

    // As after a power cut, but the next time the app opens, the system turns its own files down for a while, as a real console has been seen
    // to for a few seconds at start-up. That's taken to mean neither that it's being set up afresh, nor that the last job left nothing to deal with.
    auto refusedAtStart = [&] {
        auto console = freshConsole();
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            person.press(ButtonX);
            person.press(ButtonA);
            person.waitFor([&] { return app.snapshot().label.find("Checking every byte") == 0 || !app.busy(); }, 600);
            console->losePowerAt(1);
            person.waitUntilIdle();
        }
        console->losePowerAt(0);
        std::string settingsHere = root + "/data/settings.txt", journalHere = root + "/data/journal/journal.txt";
        report("a power cut while checking leaves a journal", sys::exists(journalHere));
        // The app keeps its settings in two places, and the other one isn't turned down.
        if (!sys::exists(root + "/config/settings.txt"))
            writeText(root + "/config/settings.txt", readText(settingsHere));
        _putenv_s("HBP_TEST_REFUSE", (settingsHere + "|" + journalHere).c_str());
        auto restarted = std::make_unique<SimConsole>(consoleFolder, keyset);
        appearance(*restarted);
        HostPlatform platform(root, keys, restarted.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.settle();
        person.waitUntilIdle();
        report("with its own files turned down as it opens, the welcome doesn't come back", app.snapshot().screen != "Welcome", app.snapshot().screen);
        report("it says what the last job left will be dealt with before anything is replaced", app.snapshot().dialogText.find("before anything is replaced") != std::string::npos,
            app.snapshot().dialog + ": " + app.snapshot().dialogText);
        person.shot("real_refused_at_start");
        _putenv_s("HBP_TEST_REFUSE", "");
        report("and the journal is still there", sys::exists(journalHere));
        if (!app.snapshot().dialog.empty())
            person.press(ButtonA);
        person.waitUntilIdle();
        // Once the files are let through, the next job deals with the last one first.
        person.press(ButtonX);
        person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 60);
        bool asked = app.snapshot().dialog.find("Compress") == 0;
        report("compressing again is asked about", asked, app.snapshot().dialog);
        if (asked) {
            person.press(ButtonA);
            person.waitUntilIdle();
        }
        healthy(*restarted, "once its files are let through and a job has run");
        // Back to the list from what the job did.
        if (app.snapshot().screen == "Results") {
            person.press(ButtonA);
            person.settle();
        }

        // A folder the system turns down while the SD card is gone through is tried again at the end, and if it's still turned down, the log
        // names it rather than passing it off as empty.
        std::string turnedDown = root + "/sd/turned down";
        sys::makeDirectories(turnedDown);
        _putenv_s("HBP_TEST_REFUSE", turnedDown.c_str());
        goToTab(person, app, 1);
        person.waitUntilIdle();
        chooseInMenu(person, app, "Refresh the list");
        person.waitFor([&] { return app.snapshot().scanning; }, 10);
        person.waitUntilIdle();
        _putenv_s("HBP_TEST_REFUSE", "");
        std::string log = readText(root + "/data/log.txt");
        report("a folder the system turns down is tried again at the end", log.find("Trying 1 folders again that the system turned down") != std::string::npos);
        report("and the log names it rather than passing it off as empty", log.find("Couldn't look in 1 folders") != std::string::npos && log.find("turned down") != std::string::npos);
        sys::removeTree(turnedDown);
    };

    // A game file installed from here is done, until its game goes from the console, whether it's uninstalled from here or from anywhere else:
    // then it's to do again, ticked, without the list being looked for again. It used to stay as installed until then.
    auto goneFromConsole = [&] {
        wipeData(root);
        std::string games = root + "/sd/games", consoleHere = root + "/console_gone", keptAside = root + "/games kept aside";
        // What's on the card for the other checks is put back afterwards, as they go on from it.
        sys::removeTree(keptAside);
        if (sys::isDirectory(games))
            sys::renameFile(games, keptAside);
        sys::removeTree(games);
        sys::removeTree(consoleHere);
        sys::makeDirectories(games);
        {
            Bytes bytes = readWhole(nsps.size() > 2 ? nsps[2] : nsps[0]);
            sys::File out(games + "/Gone Test.nsp", sys::File::Mode::Create);
            out.write(0, bytes.data(), bytes.size());
        }
        writeText(root + "/data/settings.txt", "welcomed=1\nproblemsasked=1\nmeasureall=0\ndeleteasked=1\ninstallgamefiles=0\nminimumsaving=0\ncheckwritten=0\nhidecompressed=0\nwritelog=1\n"
                                               "frompcnetwork=0\nfrompccable=0\nnspfolder=" + games + "\n");
        auto fileRow = [&](App& app) {
            for (auto& row : app.snapshot().all) {
                if (row.tab == 1 && row.mainFile.find("[LZ4]") != std::string::npos)
                    return row;
            }
            return App::FullRow{};
        };
        auto said = [](const App::FullRow& row) {
            return format("%s: %s, %s, ticked %d, to do %d, done %d", row.mainFile.c_str(), row.second.c_str(), row.status.c_str(), row.ticked ? 1 : 0, row.inToDo ? 1 : 0, row.inDone ? 1 : 0);
        };
        // X waits for whatever's going on to give way before it asks, so the question is waited for, and what was asked is said.
        auto doTheTicked = [&](Person& person, App& app) {
            person.press(ButtonX);
            person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 60);
            std::string asked = app.snapshot().dialog;
            person.press(ButtonA);
            person.waitUntilIdle();
            std::string outcome = app.snapshot().resultsTitle;
            for (auto& line : app.snapshot().results)
                outcome += " | " + line;
            if (app.snapshot().screen == "Results")
                person.press(ButtonA);
            person.settle();
            return asked + " -> " + outcome;
        };
        {
            auto console = std::make_unique<SimConsole>(consoleHere, keyset);
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToTab(person, app, 1);
            person.waitUntilIdle();
            person.settle();
            // A compressed NSP of it first, then installed from that.
            std::string made = doTheTicked(person, app);
            report("gone from the console: a compressed NSP of the game gets made", made.find("-> Finished") != std::string::npos, made);
            goToOption(person, app, "Game files and folders");
            person.press(ButtonRight);
            goToTab(person, app, 1);
            person.waitUntilIdle();
            person.settle();
            App::FullRow row = fileRow(app);
            report("it's ready to install", row.status == "Ready to install" && row.ticked, said(row));
            std::string installed = doTheTicked(person, app);
            row = fileRow(app);
            report("installed, it's done", row.inDone && !row.inToDo && console->listApplications().size() == 1, installed + "; " + said(row));

            goToTab(person, app, 0);
            person.waitUntilIdle();
            person.settle();
            chooseInMenu(person, app, "Uninstall this game");
            person.press(ButtonRight);
            person.press(ButtonA);
            person.waitUntilIdle();
            if (app.snapshot().screen == "Results")
                person.press(ButtonA);
            person.settle();
            goToTab(person, app, 1);
            person.waitUntilIdle();
            person.settle();
            row = fileRow(app);
            report("uninstalled from here, its file is to do again, and ticked", console->listApplications().empty() && row.inToDo && !row.inDone && row.ticked &&
                row.status == "Ready to install", said(row));

            std::string again = doTheTicked(person, app);
            report("installed again", console->listApplications().size() == 1, again + "; " + said(fileRow(app)));
        }
        // Then uninstalled while the app is closed, as from the HOME menu.
        {
            SimConsole console(consoleHere, keyset);
            for (u64 application : console.listApplications()) {
                InstalledGame game = console.describe(application);
                console.setRecords(application, {});
                std::set<std::string> gone;
                for (auto& meta : game.metas) {
                    console.removeMeta(meta.storage, meta.key);
                    for (auto& content : meta.contents) {
                        if (gone.insert(content.id.hex()).second && console.hasContent(meta.storage, content.id))
                            console.deleteContent(meta.storage, content.id);
                    }
                }
            }
        }
        {
            auto console = std::make_unique<SimConsole>(consoleHere, keyset);
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToTab(person, app, 1);
            person.waitUntilIdle();
            person.settle();
            App::FullRow row = fileRow(app);
            report("uninstalled elsewhere, its file is to do again as the app opens, without looking again", console->listApplications().empty() && row.inToDo &&
                !row.inDone && row.ticked && row.status == "Ready to install", said(row));
        }
        sys::removeTree(consoleHere);
        sys::removeTree(games);
        if (sys::isDirectory(keptAside))
            sys::renameFile(keptAside, games);
    };

    // Saving never goes over a file that's there. A game's NSP saved where a file has the name it gets, as an original game named the way this
    // names what it makes can, comes out as "… (2).nsp". A last rename the card turns down is asked again, and if it's turned down every time,
    // the job fails with nothing that was there gone. And a game folder the card won't let all of be read isn't built without what it couldn't
    // read: it used to be, with a refused romfs folder left out altogether.
    auto savedUnderAFreeName = [&] {
        wipeData(root);
        std::string games = root + "/sd/games", saved = root + "/sd/saved names", consoleHere = root + "/console_names", keptAside = root + "/games kept aside";
        sys::removeTree(keptAside);
        if (sys::isDirectory(games))
            sys::renameFile(games, keptAside);
        sys::removeTree(games);
        sys::removeTree(saved);
        sys::removeTree(consoleHere);
        sys::makeDirectories(games);
        sys::makeDirectories(saved);
        std::string source = nsps.size() > 2 ? nsps[2] : nsps[0];
        auto putGame = [&] {
            Bytes bytes = readWhole(source);
            sys::File out(games + "/Name Test.nsp", sys::File::Mode::Create);
            out.write(0, bytes.data(), bytes.size());
        };
        putGame();
        std::string expected;
        {
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(source));
            set.group();
            expected = outputFileName(set.jobs.at(0), set, ConvertOptions());
        }
        std::string stem = expected.substr(0, expected.size() - 4), second = saved + "/" + stem + " (2).nsp";
        writeText(saved + "/" + expected, "somebody else's file");
        writeText(root + "/data/settings.txt", "welcomed=1\nproblemsasked=1\nmeasureall=0\ndeleteasked=1\ninstallgamefiles=0\nminimumsaving=0\ncheckwritten=0\nhidecompressed=0\nwritelog=1\n"
                                               "frompcnetwork=0\nfrompccable=0\nfilesfolder=" + games + "\nnspfolder=" + saved + "\n");
        auto inSaved = [&]() -> std::string {
            std::vector<std::string> names;
            for (auto& entry : sys::list(saved))
                names.push_back(entry.name);
            std::sort(names.begin(), names.end());
            std::string all;
            for (auto& name : names)
                all += (all.empty() ? "" : ", ") + name;
            return all;
        };
        auto isNsp = [](const std::string& path) {
            try {
                return !FileArchive(path).entries().empty();
            } catch (const ToolError&) {
                return false;
            }
        };
        // Opens the app on the list of game files, does what's ticked, and says what came of it.
        auto saveTicked = [&](const std::string& shot) {
            auto console = std::make_unique<SimConsole>(consoleHere, keyset);
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToTab(person, app, 1);
            person.waitUntilIdle();
            person.settle();
            person.press(ButtonX);
            person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 60);
            person.press(ButtonA);
            person.waitUntilIdle();
            person.shot(shot);
            std::string outcome = app.snapshot().resultsTitle;
            for (auto& line : app.snapshot().results)
                outcome += " | " + line;
            if (app.snapshot().screen == "Results")
                person.press(ButtonA);
            person.settle();
            return outcome;
        };

        std::string made = saveTicked("names_saved_beside");
        // A job that went through is "Finished"; one that didn't, "Finished, with problems".
        auto wentThrough = [](const std::string& outcome) { return outcome.rfind("Finished |", 0) == 0; };
        report("saved where a file has the name it gets, it's saved as '… (2).nsp'", wentThrough(made) && isNsp(second), made + "; " + inSaved());
        report("and the file that was there is as it was", readText(saved + "/" + expected) == "somebody else's file", inSaved());

        // The last rename turned down twice: asked again, a moment later, it goes through.
        sys::removeFile(second);
        _putenv_s("HBP_TEST_REFUSE_RENAME", second.c_str());
        _putenv_s("HBP_TEST_REFUSE_RENAME_TIMES", "2");
        made = saveTicked("names_rename_refused_twice");
        report("a last rename the card turns down twice is asked again, and goes through", wentThrough(made) && isNsp(second), made + "; " + inSaved());

        // Turned down every time: the job fails, and nothing that was there goes, nor is anything left half done.
        sys::removeFile(second);
        _putenv_s("HBP_TEST_REFUSE_RENAME", (saved + "/" + stem + " (2).nsp").c_str());
        _putenv_s("HBP_TEST_REFUSE_RENAME_TIMES", "99");
        made = saveTicked("names_rename_refused");
        _putenv_s("HBP_TEST_REFUSE_RENAME", "");
        report("a last rename turned down every time fails the job, with what was there as it was and nothing half done left",
               made.rfind("Finished, with problems", 0) == 0 && readText(saved + "/" + expected) == "somebody else's file" && inSaved() == expected, made + "; " + inSaved());

        // A game folder: built beside a file of the name it gets, and not at all while the card turns down part of it. Nothing of the game
        // is where NSPs go, so it's to do.
        sys::removeFile(second);
        sys::removeTree(games);
        sys::makeDirectories(games);
        std::string folder;
        {
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(source));
            set.group();
            folder = extractTitle(set.jobs.at(0), set, games).path;
        }
        sys::makeDirectories(folder + "/romfs/extra");
        writeText(folder + "/romfs/extra/kept.txt", "a file in a folder of its own");
        _putenv_s("HBP_TEST_REFUSE", (folder + "/romfs").c_str());
        made = saveTicked("names_romfs_refused");
        _putenv_s("HBP_TEST_REFUSE", "");
        // Found out when it's opened for the list, or when it's built: either way, nothing's made of part of it.
        report("a game folder whose romfs the card won't answer about isn't built without it", !wentThrough(made) && inSaved() == expected, made + "; " + inSaved());
        _putenv_s("HBP_TEST_REFUSE", (folder + "/romfs/extra").c_str());
        made = saveTicked("names_romfs_part_refused");
        _putenv_s("HBP_TEST_REFUSE", "");
        report("nor one with a folder in it the card won't list", !wentThrough(made) && inSaved() == expected, made + "; " + inSaved());
        made = saveTicked("names_folder_saved");
        bool built = isNsp(second);
        bool hasExtra = false;
        if (built) {
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(second));
            set.group();
            std::string out = root + "/names_check_extract";
            sys::removeTree(out);
            hasExtra = readText(extractTitle(set.jobs.at(0), set, out).path + "/romfs/extra/kept.txt") == "a file in a folder of its own";
            sys::removeTree(out);
        }
        report("and with the card letting it, it's built beside the file of its name, as '… (2).nsp', with all of it in it, as what was turned down isn't remembered",
               wentThrough(made) && built && hasExtra &&
               readText(saved + "/" + expected) == "somebody else's file", made + "; " + inSaved());

        // An extraction that didn't finish leaves its note in the folder, which then isn't a game, however much of one it looks like.
        sys::removeTree(saved);
        sys::makeDirectories(saved);
        writeText(folder + "/" + ExtractedGame::NotFinished, ExtractedGame::NotFinishedText);
        {
            auto console = std::make_unique<SimConsole>(consoleHere, keyset);
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToTab(person, app, 1);
            person.waitUntilIdle();
            person.settle();
            int listed = 0;
            for (auto& row : app.snapshot().all)
                listed += row.tab == 1 && !row.extracted.empty() ? 1 : 0;
            report("a folder an extraction didn't finish in, with its note, isn't listed as a game", listed == 0 && !ExtractedGame::isOne(folder), format("%d listed", listed));
        }

        sys::removeTree(consoleHere);
        sys::removeTree(games);
        sys::removeTree(saved);
        if (sys::isDirectory(keptAside))
            sys::renameFile(keptAside, games);
    };

    // Saving NSPs, a game's DLC goes into its file with it, as on the PC, or with Game and DLC in one file off, each into a file of its own,
    // even one with nothing to compress. That file, opened again, is the game, compressed and done, with its DLC. DLC without their game go
    // together into one file, and a copy of the game without its DLC is made again with them.
    auto dlcInGameFile = [&] {
        const char* inputs = getenv("HBP_TEST_DLC_INPUTS");
        if (!inputs || !*inputs || !sys::exists(std::string(inputs) + "/dlc_small.nsp")) {
            printf("  skipped: Game and DLC in one file needs HBP_TEST_DLC_INPUTS, from make_patch_test.py's dlcs\n");
            return;
        }
        std::string from = inputs;
        std::string games = root + "/sd/dlc games", saved = games + "/saved", consoleHere = root + "/console_dlc", logHere = root + "/data/log.txt";
        auto copyTo = [&](const std::string& source, const std::string& to) {
            Bytes bytes = readWhole(source);
            sys::File out(to, sys::File::Mode::Create);
            out.write(0, bytes.data(), bytes.size());
        };
        // The titles in a file, by ID, lowest first.
        auto titlesIn = [&](const std::string& path) {
            std::vector<u64> ids;
            try {
                TitleSet set(keyset);
                set.add(std::make_shared<FileArchive>(path));
                set.group();
                for (auto& job : set.jobs)
                    ids.push_back(job.main->meta.titleId);
            } catch (const ToolError&) {
            }
            std::sort(ids.begin(), ids.end());
            return ids;
        };
        auto idsSaid = [](const std::vector<u64>& ids) {
            std::string said;
            for (u64 id : ids)
                said += (said.empty() ? "" : " ") + format("%016llX", (unsigned long long)id);
            return said;
        };
        // What's saved, each file with the titles in it.
        auto savedFiles = [&] {
            std::vector<std::string> names;
            for (auto& entry : sys::list(saved))
                names.push_back(entry.name);
            std::sort(names.begin(), names.end());
            return names;
        };
        auto savedSaid = [&] {
            std::string said;
            for (auto& name : savedFiles())
                said += (said.empty() ? "" : "; ") + name + ": " + idsSaid(titlesIn(saved + "/" + name));
            return said.empty() ? std::string("nothing saved") : said;
        };
        u64 game = titlesIn(from + "/base.nsp").at(0), dlc = titlesIn(from + "/dlc.nsp").at(0), smallDlc = titlesIn(from + "/dlc_small.nsp").at(0);
        // Settings, and with keepData, what the app found out and remembered before, as when it's opened again.
        auto settingsWith = [&](const std::string& more, bool keepData = false) {
            // The app's backup of its settings ends the way the app writes it, so it would be read before these: it goes either way.
            if (!keepData)
                wipeData(root);
            else
                sys::removeFile(root + "/config/settings.txt");
            writeText(root + "/data/settings.txt", "welcomed=1\nproblemsasked=1\nmeasureall=0\ndeleteasked=1\nminimumsaving=0\nhidecompressed=0\nmoresettings=1\nwritelog=1\n"
                                                   "frompcnetwork=0\nfrompccable=0\nfilesfolder=" + games + "\nnspfolder=" + saved + "\n" + more);
        };
        // Opens the app on a list, does what's ticked there, and says what came of it. Tick all ticks every game listed by hand, as done or
        // not. What the list showed by itself, and after the job, are kept.
        App::Snapshot listedFirst, listedAfter;
        auto doTicked = [&](SimConsole& console, int tab, const std::string& shot, bool backToList = false, bool shotFirst = false) {
            HostPlatform platform(root, keys, &console);
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToTab(person, app, tab);
            person.waitUntilIdle();
            person.settle();
            listedFirst = app.snapshot();
            if (shotFirst)
                person.shot(shot + "_first");
            if (app.snapshot().tickHint == "Tick all")
                person.press(ButtonY);
            person.press(ButtonX);
            person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 60);
            person.press(ButtonA);
            person.waitUntilIdle();
            person.shot(shot);
            listedAfter = app.snapshot();
            std::string outcome = app.snapshot().resultsTitle;
            for (auto& line : app.snapshot().results)
                outcome += " | " + line;
            // And the list, as it's drawn afterwards.
            if (backToList && app.snapshot().screen == "Results") {
                person.press(ButtonA);
                person.settle();
                person.shot(shot + "_list");
            }
            return outcome;
        };
        // The one game on the list of game files, as a snapshot has it: where it's listed, what its second line says, its details and whether
        // it counts as compressed.
        struct Listed {
            bool one = false, toDo = false, done = false, ticked = false, compressed = false;
            std::string mainFile, second, detail, said, state, note;
        };
        auto gameIn = [](const App::Snapshot& s) {
            Listed got;
            int count = 0;
            for (auto& row : s.all) {
                if (row.tab != 1)
                    continue;
                count++;
                got.toDo = row.inToDo;
                got.done = row.inDone;
                got.ticked = row.ticked;
                got.mainFile = row.mainFile;
                got.second = row.second;
                got.state = row.state;
                for (auto& shown : s.rows) {
                    if (shown.name == row.name) {
                        got.detail = shown.detail;
                        got.compressed = shown.compressed;
                        got.note = shown.note;
                    }
                }
                got.said += row.name + ": \"" + got.detail + "\", \"" + row.second + "\"" + (row.inDone ? " (Done)" : "") + (row.inToDo ? " (To do)" : "") +
                            (row.ticked ? " (ticked)" : "") + (got.compressed ? " (compressed)" : "") + " (" + row.state + "), " + app::fileNameOf(row.mainFile) + "; ";
            }
            got.one = count == 1;
            return got;
        };
        // What's saved, each file with the titles in it, lowest first.
        auto savedHeld = [&] {
            std::vector<std::vector<u64>> held;
            for (auto& name : savedFiles())
                held.push_back(titlesIn(saved + "/" + name));
            std::sort(held.begin(), held.end());
            return held;
        };
        // A copy of the game and its update as this tool makes one, holding these DLC too.
        auto copyOf = [&](const std::string& to, const std::vector<std::string>& dlcFiles) {
            TitleSet set(keyset);
            for (auto& name : std::vector<std::string>{"base.nsp", "update.nsp"})
                set.add(std::make_shared<FileArchive>(from + "/" + name));
            for (auto& name : dlcFiles)
                set.add(std::make_shared<FileArchive>(from + "/" + name));
            set.group();
            std::vector<const TitleJob*> titles;
            int contents = 0;
            for (auto& job : set.jobs) {
                titles.push_back(&job);
                contents += (int)job.contents.size();
            }
            std::sort(titles.begin(), titles.end(), [](const TitleJob* a, const TitleJob* b) { return a->isBaseGame() != b->isBaseGame() ? a->isBaseGame() : a->main->meta.titleId < b->main->meta.titleId; });
            NspOutput nsp(to, contents, 0, true, (int)titles.size());
            TitleConverter converter(ConvertOptions(), keyset);
            for (auto* title : titles)
                converter.run(*title, nsp);
        };
        auto wentThrough = [](const std::string& outcome) { return outcome.rfind("Finished |", 0) == 0; };
        auto fresh = [&] {
            sys::removeTree(games);
            sys::removeTree(consoleHere);
            sys::makeDirectories(games);
        };

        // From the SD card, with it on: one file holding the game and its DLC.
        fresh();
        copyTo(from + "/base.nsp", games + "/base.nsp");
        copyTo(from + "/update.nsp", games + "/update.nsp");
        copyTo(from + "/dlc_small.nsp", games + "/dlc_small.nsp");
        settingsWith("installgamefiles=0\n");
        std::string outcome;
        {
            SimConsole console(consoleHere, keyset);
            outcome = doTicked(console, 1, "dlcfile_saved");
        }
        auto files = savedFiles();
        report("saving a game, its update and a DLC with nothing to compress makes one file, holding the game and the DLC",
               wentThrough(outcome) && files.size() == 1 && titlesIn(saved + "/" + files[0]) == std::vector<u64>{game, smallDlc}, outcome + "; " + savedSaid());
        std::string made = files.size() == 1 ? saved + "/" + files[0] : "";

        // Opened again beside what it was made from, it's the game, compressed, under Done with its DLC, as it is without being read again.
        settingsWith("installgamefiles=0\n");
        {
            SimConsole console(consoleHere, keyset);
            HostPlatform platform(root, keys, &console);
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToTab(person, app, 1);
            person.waitUntilIdle();
            person.settle();
            std::vector<App::FullRow> rows;
            for (auto& row : app.snapshot().all) {
                if (row.tab == 1)
                    rows.push_back(row);
            }
            std::string listed;
            for (auto& row : rows)
                listed += row.name + ": " + row.second + (row.inDone ? " (Done)" : "") + (row.inToDo ? " (To do)" : "") + (row.ticked ? " (ticked)" : "") + "; ";
            bool one = rows.size() == 1;
            report("opened again, that file is one row, under Done, not ticked", one && rows[0].inDone && !rows[0].inToDo && !rows[0].ticked, listed);
            report("with its DLC, the one in that file", one && rows[0].mainFile == made && rows[0].dlcFiles == std::vector<std::string>{made},
                   one ? rows[0].mainFile + " / " + (rows[0].dlcFiles.empty() ? std::string("no DLC") : rows[0].dlcFiles[0]) : listed);
            Listed opened = gameIn(app.snapshot());
            report("which its row's details say: 1 DLC", opened.one && opened.detail.find("1 DLC") != std::string::npos && opened.detail.find("not in its file") == std::string::npos,
                   opened.said);
            report("and it's compressed as soon as it's opened, without being read", one && rows[0].alreadyCompressed && readText(logHere).find("Analyzing " + rows[0].name) == std::string::npos,
                   one && rows[0].alreadyCompressed ? "read" : "not compressed");
            person.shot("dlcfile_opened_again");
        }
        // Ticked and done again, it makes nothing new.
        {
            SimConsole console(consoleHere, keyset);
            outcome = doTicked(console, 1, "dlcfile_saved_again");
        }
        report("and the same files a second time make nothing new", savedFiles() == files, outcome + "; " + savedSaid());

        // With it off, each DLC gets a file of its own, even one with nothing to compress.
        sys::removeTree(saved);
        settingsWith("installgamefiles=0\ndlcingamefile=0\n");
        {
            SimConsole console(consoleHere, keyset);
            outcome = doTicked(console, 1, "dlcfile_off");
        }
        files = savedFiles();
        std::vector<std::vector<u64>> held;
        for (auto& name : files)
            held.push_back(titlesIn(saved + "/" + name));
        std::sort(held.begin(), held.end());
        report("and with Game and DLC in one file off, two files, the game's and the DLC's",
               wentThrough(outcome) && held == std::vector<std::vector<u64>>{{game}, {smallDlc}}, outcome + "; " + savedSaid());

        // A merged file, with the game, its update and both DLC in it, comes out as one file holding all of them.
        fresh();
        copyTo(from + "/merged.nsp", games + "/merged.nsp");
        settingsWith("installgamefiles=0\n");
        {
            SimConsole console(consoleHere, keyset);
            outcome = doTicked(console, 1, "dlcfile_merged");
        }
        files = savedFiles();
        report("a merged file of a game, its update and two DLC comes out as one file holding all three",
               wentThrough(outcome) && files.size() == 1 && titlesIn(saved + "/" + files[0]) == std::vector<u64>{game, dlc, smallDlc}, outcome + "; " + savedSaid());

        // A DLC this tool compressed before, given with its game: with the setting off it's a file of its own already and stays as it is, with
        // it on it goes into the game's file, and with compression off it's repacked like the rest, as on the PC.
        fresh();
        copyTo(from + "/base.nsp", games + "/base.nsp");
        copyTo(from + "/update.nsp", games + "/update.nsp");
        {
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(from + "/dlc.nsp"));
            set.group();
            ConvertOptions options;
            NspOutput nsp(games + "/" + outputFileName(set.jobs.at(0), set, options), (int)set.jobs.at(0).contents.size(), 0, true);
            TitleConverter(options, keyset).run(set.jobs.at(0), nsp);
        }
        struct Case {
            const char* settings;
            const char* shot;
            const char* said;
            std::vector<std::vector<u64>> expected;
        };
        for (const Case& each : {Case{"dlcingamefile=0\n", "dlcfile_older_off", "with it off, a DLC this tool compressed before stays as it is, and only the game's file is made", {{game}}},
                                 Case{"", "dlcfile_older_on", "with it on, that DLC goes into the game's file", {{game, dlc}}},
                                 Case{"dlcingamefile=0\ncompress=0\n", "dlcfile_older_repacked", "and with it off and compression off, it's repacked in a file of its own", {{game}, {dlc}}}}) {
            sys::removeTree(saved);
            settingsWith(std::string("installgamefiles=0\n") + each.settings);
            {
                SimConsole console(consoleHere, keyset);
                outcome = doTicked(console, 1, each.shot);
            }
            std::vector<std::vector<u64>> got;
            for (auto& name : savedFiles())
                got.push_back(titlesIn(saved + "/" + name));
            std::sort(got.begin(), got.end());
            report(each.said, wentThrough(outcome) && got == each.expected, outcome + "; " + savedSaid());
        }

        // A damaged DLC stops the game's file being made, and says it's the DLC.
        fresh();
        copyTo(from + "/base.nsp", games + "/base.nsp");
        copyTo(from + "/update.nsp", games + "/update.nsp");
        {
            Bytes bytes = readWhole(from + "/dlc.nsp");
            bytes[bytes.size() - 30000] ^= 0xFF;
            sys::File out(games + "/dlc.nsp", sys::File::Mode::Create);
            out.write(0, bytes.data(), bytes.size());
        }
        settingsWith("installgamefiles=0\n");
        {
            SimConsole console(consoleHere, keyset);
            outcome = doTicked(console, 1, "dlcfile_damaged", true);
        }
        std::string named = format("Its DLC %016llX in 'dlc.nsp'", (unsigned long long)dlc);
        report("a damaged DLC is named as the DLC, not the game, and nothing is left saved", outcome.find(named) != std::string::npos && savedFiles().empty(),
               outcome + "; " + savedSaid());
        Listed damaged = gameIn(listedAfter);
        report("and its row says a DLC is damaged, in a few words", damaged.second == "A DLC is damaged: get it again, or turn off Game and DLC in one file.",
               damaged.said);
        // Opened again, with what it remembered, the game is still failed and unticked, saying why, so doing what's ticked does nothing: its
        // file isn't written and thrown away each time. What it shows, and what doing what's ticked came to.
        auto openedAgain = [&](const std::string& shot) {
            SimConsole console(consoleHere, keyset);
            HostPlatform platform(root, keys, &console);
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToTab(person, app, 1);
            person.waitUntilIdle();
            person.settle();
            person.shot(shot);
            Listed got = gameIn(app.snapshot());
            person.press(ButtonX);
            person.settle();
            if (!app.snapshot().dialog.empty())
                person.press(ButtonA);
            person.waitUntilIdle();
            return std::make_pair(got, app.snapshot().resultsTitle);
        };
        std::string damagedSaid = "A DLC is damaged: get it again, or turn off Game and DLC in one file.";
        auto [failedAgain, didAgain] = openedAgain("dlcfile_damaged_again");
        report("opened again, it's still failed and unticked, saying why, and doing what's ticked writes nothing",
               failedAgain.one && failedAgain.state == "Failed" && !failedAgain.ticked && failedAgain.second == damagedSaid && didAgain.empty() && savedFiles().empty(),
               failedAgain.said + "done: " + didAgain + "; " + savedSaid());
        // With the setting off, the game's file can be made without its DLC, so it's tried again.
        settingsWith("installgamefiles=0\ndlcingamefile=0\n", true);
        Listed triedAgain = openedAgain("dlcfile_damaged_off").first;
        report("and with Game and DLC in one file off, it's to do again", triedAgain.one && triedAgain.state != "Failed" && triedAgain.ticked, triedAgain.said);

        // The same for DLC without their game, which go into one file: the damaged one is the first in it, so it's just damaged.
        fresh();
        copyTo(from + "/dlc_small.nsp", games + "/dlc_small.nsp");
        {
            Bytes bytes = readWhole(from + "/dlc.nsp");
            bytes[bytes.size() - 30000] ^= 0xFF;
            sys::File out(games + "/dlc.nsp", sys::File::Mode::Create);
            out.write(0, bytes.data(), bytes.size());
        }
        settingsWith("installgamefiles=0\n");
        {
            SimConsole console(consoleHere, keyset);
            outcome = doTicked(console, 1, "dlcfile_damaged_alone");
        }
        Listed alone = gameIn(listedAfter);
        auto [aloneAgain, aloneDid] = openedAgain("dlcfile_damaged_alone_again");
        report("DLC without their game, one of them damaged, fail, and opened again they're still failed and unticked, and nothing is written",
               alone.state == "Failed" && aloneAgain.one && aloneAgain.state == "Failed" && !aloneAgain.ticked && aloneAgain.second == "Damaged: dump or download it again." &&
                   aloneDid.empty() && savedFiles().empty(),
               alone.said + " then " + aloneAgain.said + "done: " + aloneDid + "; " + savedSaid());

        // DLC without their game go together into one file, named after the first, and the same files a second time make nothing new. With it
        // off, the DLC with something to compress gets a file of its own, and the one with nothing to compress, a file of its own already, stays
        // as it is.
        fresh();
        copyTo(from + "/dlc.nsp", games + "/dlc.nsp");
        copyTo(from + "/dlc_small.nsp", games + "/dlc_small.nsp");
        settingsWith("installgamefiles=0\n");
        {
            SimConsole console(consoleHere, keyset);
            outcome = doTicked(console, 1, "dlcfile_alone");
        }
        files = savedFiles();
        report("DLC without their game make one file holding both", wentThrough(outcome) && savedHeld() == std::vector<std::vector<u64>>{{dlc, smallDlc}},
               outcome + "; " + savedSaid());
        {
            SimConsole console(consoleHere, keyset);
            outcome = doTicked(console, 1, "dlcfile_alone_again");
        }
        report("and the same files a second time make nothing new", savedFiles() == files, outcome + "; " + savedSaid());
        sys::removeTree(saved);
        settingsWith("installgamefiles=0\ndlcingamefile=0\n");
        {
            SimConsole console(consoleHere, keyset);
            outcome = doTicked(console, 1, "dlcfile_alone_off");
        }
        report("with it off, the DLC with something to compress gets a file of its own, and the other stays as it is",
               wentThrough(outcome) && savedHeld() == std::vector<std::vector<u64>>{{dlc}}, outcome + "; " + savedSaid());

        // Two DLC without their game with nothing to compress save nothing, so they're under Done as any game that saves nothing is, but they
        // don't count as compressed, as no one file holds them. Ticked, they make one file holding both, and the same files a second time make
        // nothing new.
        std::string small2 = from + "/dlc_small2.nsp";
        report("the DLC inputs have make_patch_test.py's dlc_small2.nsp", sys::exists(small2), small2);
        if (sys::exists(small2)) {
            u64 smallDlc2 = titlesIn(small2).at(0);
            fresh();
            copyTo(from + "/dlc_small.nsp", games + "/dlc_small.nsp");
            copyTo(small2, games + "/dlc_small2.nsp");
            settingsWith("installgamefiles=0\n");
            {
                SimConsole console(consoleHere, keyset);
                outcome = doTicked(console, 1, "dlcfile_plain");
            }
            Listed plain = gameIn(listedFirst);
            files = savedFiles();
            report("two DLC without their game with nothing to compress don't count as compressed, and ticked they make one file holding both",
                   plain.one && !plain.compressed && wentThrough(outcome) && savedHeld() == std::vector<std::vector<u64>>{{smallDlc, smallDlc2}},
                   plain.said + outcome + "; " + savedSaid());
            // With no game to say its DLC aren't in its file, their line says why they're settled, as any other's does.
            report("and their line says they've nothing to gain", startsWith(plain.second, "Nothing to gain"), plain.said);
            {
                SimConsole console(consoleHere, keyset);
                outcome = doTicked(console, 1, "dlcfile_plain_again");
            }
            report("and the same files a second time make nothing new", savedFiles() == files, outcome + "; " + savedSaid());
        }

        // A file that holds both DLC without their game already, as another tool packs them, repacked here with compression off: one has something
        // to compress, and the file made of it holds the other one too. The same files a second time make nothing new.
        fresh();
        {
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(from + "/dlc.nsp"));
            set.add(std::make_shared<FileArchive>(from + "/dlc_small.nsp"));
            set.group();
            std::vector<const TitleJob*> titles;
            int contents = 0;
            for (auto& job : set.jobs) {
                titles.push_back(&job);
                contents += (int)job.contents.size();
            }
            std::sort(titles.begin(), titles.end(), [](const TitleJob* a, const TitleJob* b) { return a->main->meta.titleId < b->main->meta.titleId; });
            ConvertOptions options;
            options.compress = false;
            NspOutput nsp(games + "/dlc pack.nsp", contents, 0, true, (int)titles.size());
            TitleConverter converter(options, keyset);
            for (auto* title : titles)
                converter.run(*title, nsp);
        }
        settingsWith("installgamefiles=0\n");
        {
            SimConsole console(consoleHere, keyset);
            outcome = doTicked(console, 1, "dlcfile_pack");
        }
        files = savedFiles();
        report("a file holding both DLC without their game, one worth compressing, makes one file holding both",
               wentThrough(outcome) && savedHeld() == std::vector<std::vector<u64>>{{dlc, smallDlc}}, outcome + "; " + savedSaid());
        {
            SimConsole console(consoleHere, keyset);
            outcome = doTicked(console, 1, "dlcfile_pack_again");
        }
        report("and the same files a second time make nothing new", savedFiles() == files, outcome + "; " + savedSaid());

        // A copy this tool made of the game and its update, without its DLC, given with a DLC and not the original: it's to do, saying its DLC
        // isn't in its file, and saving it makes the game's file again with the DLC. The same files a second time make nothing new.
        // Its file has a long name, as the copies people keep often do: the line on the list still shows that its DLC isn't in its file.
        fresh();
        copyOf(games + "/LZ Test Complete Edition with Every Extra and Bonus Content [0100ABCD12340000][v65536][LZ4].nsp", {});
        copyTo(from + "/dlc.nsp", games + "/dlc.nsp");
        settingsWith("installgamefiles=0\n");
        {
            SimConsole console(consoleHere, keyset);
            outcome = doTicked(console, 1, "dlcfile_copy", false, true);
        }
        Listed copy = gameIn(listedFirst);
        report("a copy whose DLC isn't in its file is to do and ticked, and says so",
               copy.one && copy.toDo && copy.ticked && !copy.compressed && copy.second.find("1 DLC not in its file") != std::string::npos, copy.said);
        // As the line under its name is drawn, 500 wide at 14.5, a little smaller first, then cut short (Canvas::textFit), with the font that
        // stands in for the console's here.
        std::string drawn;
        {
            Bytes face = readWhole("C:/Windows/Fonts/segoeui.ttf");
            Font font;
            font.addFace(face.data(), face.size());
            float size = 14.5f;
            while (size > 14.5f * 0.82f && font.measure(copy.second, size) > 500)
                size -= 0.5f;
            drawn = fitText(font, copy.second, size, 500);
        }
        report("and with a long file name, its line still shows that", drawn.find("1 DLC not in its file") != std::string::npos, drawn);
        files = savedFiles();
        report("and saving it makes one file holding the game and the DLC", wentThrough(outcome) && savedHeld() == std::vector<std::vector<u64>>{{game, dlc}},
               outcome + "; " + savedSaid());
        {
            SimConsole console(consoleHere, keyset);
            outcome = doTicked(console, 1, "dlcfile_copy_again");
        }
        report("and the same files a second time make nothing new", savedFiles() == files, outcome + "; " + savedSaid());

        // Given with a DLC with nothing to compress, the copy saves nothing, so it's under Done as any game that saves nothing is. It doesn't
        // count as compressed, and says its DLC isn't in its file, which turning Game and DLC in one file off changes and on again changes back.
        // Ticked, it makes the game's file again with the DLC.
        fresh();
        copyOf(games + "/a copy.nsp", {});
        copyTo(from + "/dlc_small.nsp", games + "/dlc_small.nsp");
        settingsWith("installgamefiles=0\n");
        {
            SimConsole console(consoleHere, keyset);
            HostPlatform platform(root, keys, &console);
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToTab(person, app, 1);
            person.waitUntilIdle();
            person.settle();
            Listed on = gameIn(app.snapshot()), off, back, installing, saving;
            openSettings(person, app);
            person.settle();
            if (optionValue(app, "Game and DLC in one file") != "(no such option)") {
                goToOption(person, app, "Game and DLC in one file");
                person.press(ButtonA);
                person.waitUntilIdle();
                person.settle();
                off = gameIn(app.snapshot());
                person.press(ButtonA);
                person.waitUntilIdle();
                person.settle();
                back = gameIn(app.snapshot());
            }
            // Game files installed, the DLC goes on the console apart from the game anyway, so the copy is ready to install, and saving
            // again, it's as it was.
            goToOption(person, app, "Game files and folders");
            person.press(ButtonA);
            person.waitUntilIdle();
            person.settle();
            installing = gameIn(app.snapshot());
            person.press(ButtonA);
            person.waitUntilIdle();
            person.settle();
            saving = gameIn(app.snapshot());
            auto saysSo = [](const Listed& got) { return got.one && !got.compressed && got.second.find("1 DLC not in its file") != std::string::npos; };
            report("a copy given with a DLC with nothing to compress doesn't count as compressed, and its line says its DLC isn't in its file", saysSo(on), on.said);
            report("turning Game and DLC in one file off makes it compressed, and on again it's as it was",
                   off.one && off.compressed && off.detail.find("not in its file") == std::string::npos && saysSo(back), "off: " + off.said + " on again: " + back.said);
            report("installing game files, it's ready to install as it is, and saving them again, it's as it was",
                   installing.one && installing.ticked && startsWith(installing.second, "Compressed, not installed yet") && saysSo(saving),
                   "installing: " + installing.said + " saving: " + saving.said);
        }
        {
            SimConsole console(consoleHere, keyset);
            outcome = doTicked(console, 1, "dlcfile_copy_small");
        }
        report("and ticked, it makes one file holding the game and the DLC", wentThrough(outcome) && savedHeld() == std::vector<std::vector<u64>>{{game, smallDlc}},
               outcome + "; " + savedSaid());

        // A copy holding one of its DLC, with the other given beside it: two DLC, one not in its file, and saving makes one file holding both.
        fresh();
        copyOf(games + "/a copy.nsp", {"dlc_small.nsp"});
        copyTo(from + "/dlc.nsp", games + "/dlc.nsp");
        settingsWith("installgamefiles=0\n");
        {
            SimConsole console(consoleHere, keyset);
            outcome = doTicked(console, 1, "dlcfile_copy_one_apart");
        }
        Listed oneApart = gameIn(listedFirst);
        report("a copy holding one of its two DLC says one isn't in its file, and saving makes one file holding the game and both",
               oneApart.one && oneApart.detail.find("2 DLC, 1 not in its file") != std::string::npos && wentThrough(outcome) &&
                   savedHeld() == std::vector<std::vector<u64>>{{game, dlc, smallDlc}},
               oneApart.said + outcome + "; " + savedSaid());

        // With compression off, a copy whose DLC isn't in its file is still a file this tool compressed, which repacking only makes bigger. Asked
        // to repack it before its first look is in, that's known already, and it isn't asked about as a game without an update would be. Its DLC
        // here is one this tool compressed in a file of its own.
        fresh();
        copyOf(games + "/a copy.nsp", {});
        {
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(from + "/dlc.nsp"));
            set.group();
            ConvertOptions options;
            NspOutput nsp(games + "/" + outputFileName(set.jobs.at(0), set, options), (int)set.jobs.at(0).contents.size(), 0, true);
            TitleConverter(options, keyset).run(set.jobs.at(0), nsp);
        }
        settingsWith("installgamefiles=0\ncompress=0\n");
        _putenv_s("HBP_TEST_SLOW_LOOK", "20000");
        {
            SimConsole console(consoleHere, keyset);
            HostPlatform platform(root, keys, &console);
            App app(platform);
            app.start();
            Person person(app, shots);
            goToTab(person, app, 1);
            person.waitFor([&] { return app.snapshot().scanned[1] && gameIn(app.snapshot()).one; }, 60);
            Listed before = gameIn(app.snapshot());
            person.press(ButtonX);
            person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 60);
            std::string asked = app.snapshot().dialog;
            report("with compression off, a copy whose DLC isn't in its file isn't asked about as a game without an update", !asked.empty() && asked.find("no update") == std::string::npos,
                   before.said + "asked: " + asked);
            if (!asked.empty())
                person.press(ButtonB);
            person.settle();
        }
        _putenv_s("HBP_TEST_SLOW_LOOK", "");

        // Of two copies of the game as new, one without the DLC and one with it, the one with it is used, and it's done: nothing new is made.
        fresh();
        copyOf(games + "/a copy.nsp", {});
        copyOf(games + "/with dlc.nsp", {"dlc_small.nsp"});
        copyTo(from + "/dlc_small.nsp", games + "/dlc_small.nsp");
        settingsWith("installgamefiles=0\n");
        {
            SimConsole console(consoleHere, keyset);
            outcome = doTicked(console, 1, "dlcfile_two_copies");
        }
        Listed both = gameIn(listedFirst);
        report("of a copy without the DLC and one with it, the one with it is used, and nothing new is made",
               both.one && both.mainFile == games + "/with dlc.nsp" && both.done && savedFiles().empty(), both.said + outcome + "; " + savedSaid());

        // A copy without the DLC, given with the merged original it was made from, where the DLC are, gives way to the original, and the game's
        // file is made with the DLC.
        fresh();
        copyOf(games + "/a copy.nsp", {});
        copyTo(from + "/merged.nsp", games + "/merged.nsp");
        settingsWith("installgamefiles=0\n");
        {
            SimConsole console(consoleHere, keyset);
            outcome = doTicked(console, 1, "dlcfile_copy_original");
        }
        Listed original = gameIn(listedFirst);
        report("a copy without the DLC, given with its merged original, gives way to the original, and saving makes one file holding the game and the DLC",
               original.one && original.mainFile == games + "/merged.nsp" && wentThrough(outcome) && savedHeld() == std::vector<std::vector<u64>>{{game, dlc, smallDlc}},
               original.said + outcome + "; " + savedSaid());

        // Given with its original and its DLC in a file of its own, as 1.5.0 left them, the copy is kept, saying its DLC isn't in its file. Making
        // it again saves nothing, so it's under Done and not ticked, and ticked, it makes one file holding the game and the DLC.
        fresh();
        copyOf(games + "/a copy.nsp", {});
        copyTo(from + "/base.nsp", games + "/base.nsp");
        copyTo(from + "/update.nsp", games + "/update.nsp");
        copyTo(from + "/dlc_small.nsp", games + "/dlc_small.nsp");
        settingsWith("installgamefiles=0\n");
        {
            SimConsole console(consoleHere, keyset);
            outcome = doTicked(console, 1, "dlcfile_copy_loose");
        }
        Listed loose = gameIn(listedFirst);
        report("a copy given with its original and its DLC in a file of its own is kept, under Done and not ticked, saying its DLC isn't in its file",
               loose.one && loose.mainFile == games + "/a copy.nsp" && loose.done && !loose.ticked && loose.second.find("1 DLC not in its file") != std::string::npos, loose.said);
        report("and ticked, it makes one file holding the game and the DLC", wentThrough(outcome) && savedHeld() == std::vector<std::vector<u64>>{{game, smallDlc}},
               outcome + "; " + savedSaid());

        // With Game and DLC in one file off, the copy given with this tool's file of one DLC and the merged original, where the other DLC is: that
        // DLC has no file of its own, so the row is to do and ticked after its first look, and doing it makes a file of that DLC. Opened again,
        // nothing is written, the row is done, and doing what's ticked makes nothing new.
        fresh();
        copyOf(games + "/a copy.nsp", {});
        copyTo(from + "/merged.nsp", games + "/merged.nsp");
        {
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(from + "/dlc.nsp"));
            set.group();
            ConvertOptions options;
            NspOutput nsp(games + "/" + outputFileName(set.jobs.at(0), set, options), (int)set.jobs.at(0).contents.size(), 0, true);
            TitleConverter(options, keyset).run(set.jobs.at(0), nsp);
        }
        settingsWith("installgamefiles=0\ndlcingamefile=0\n");
        {
            SimConsole console(consoleHere, keyset);
            outcome = doTicked(console, 1, "dlcfile_merged_off");
        }
        Listed apart = gameIn(listedFirst);
        report("off, a copy whose other DLC is only inside its merged original is to do and ticked after its first look",
               apart.one && apart.toDo && apart.ticked && !apart.compressed && apart.mainFile == games + "/a copy.nsp", apart.said);
        report("and doing it makes a file of that DLC", wentThrough(outcome) && savedHeld() == std::vector<std::vector<u64>>{{smallDlc}}, outcome + "; " + savedSaid());
        // Every file in the games folder and the one files are saved in, with its size.
        auto filesHere = [&] {
            std::vector<std::string> all;
            for (auto& folder : {games, saved}) {
                for (auto& entry : sys::list(folder))
                    all.push_back(folder + "/" + entry.name + format(" %lld", (long long)entry.size));
            }
            std::sort(all.begin(), all.end());
            return all;
        };
        files = savedFiles();
        std::vector<std::string> before = filesHere();
        Listed apartAgain;
        {
            SimConsole console(consoleHere, keyset);
            HostPlatform platform(root, keys, &console);
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToTab(person, app, 1);
            person.waitUntilIdle();
            person.settle();
            apartAgain = gameIn(app.snapshot());
        }
        report("opened again with that file, nothing is written and the row is done", filesHere() == before && apartAgain.one && apartAgain.done && !apartAgain.ticked,
               apartAgain.said);
        {
            SimConsole console(consoleHere, keyset);
            outcome = doTicked(console, 1, "dlcfile_merged_off_again");
        }
        report("and doing what's ticked makes nothing new", savedFiles() == files, outcome + "; " + savedSaid());

        // A refused read never means a file isn't there. The same files with the other DLC's own file too, which the card turns down: until
        // it's read, that DLC isn't taken to have no file of its own, so with the setting on the copy isn't given up for its original, and
        // with it off no file of that DLC is made. Doing what's ticked writes nothing, and once the read goes through, still nothing.
        copyTo(from + "/dlc_small.nsp", games + "/dlc_small.nsp");
        for (const char* setting : {"", "dlcingamefile=0\n"}) {
            std::string which = *setting ? "off" : "on";
            sys::removeTree(saved);
            settingsWith(std::string("installgamefiles=0\n") + setting);
            std::vector<std::string> was = filesHere();
            _putenv_s("HBP_TEST_REFUSE", (games + "/dlc_small.nsp").c_str());
            auto [refused, didRefused] = openedAgain("dlcfile_refused_" + which);
            _putenv_s("HBP_TEST_REFUSE", "");
            auto [read, didRead] = openedAgain("dlcfile_refused_read_" + which);
            report("with it " + which + ", a DLC's own file the card turns down doesn't count as missing: the copy stays as it is, and nothing is written",
                   refused.one && refused.mainFile == games + "/a copy.nsp" && !refused.ticked && didRefused.empty() && filesHere() == was,
                   refused.said + "done: " + didRefused + "; " + savedSaid());
            report("and with it " + which + ", once that file is read, still nothing is written",
                   read.one && read.mainFile == games + "/a copy.nsp" && didRead.empty() && filesHere() == was, read.said + "done: " + didRead + "; " + savedSaid());
        }

        // An installed game with its DLC, saved as an NSP, the same: one file with it on, and with it off, a file each.
        fresh();
        std::string kept = root + "/sd/dlc one file";
        sys::removeTree(kept);
        sys::makeDirectories(kept);
        {
            SimConsole console(consoleHere, keyset);
            console.install({from + "/base.nsp", from + "/update.nsp", from + "/dlc.nsp", from + "/dlc_small.nsp"}, Storage::Sd);
            settingsWith("replace=0\n");
            outcome = doTicked(console, 0, "dlcfile_installed");
            files = savedFiles();
            report("saving an installed game with two DLC makes one file holding all three",
                   wentThrough(outcome) && files.size() == 1 && titlesIn(saved + "/" + files[0]) == std::vector<u64>{game, dlc, smallDlc}, outcome + "; " + savedSaid());
            if (files.size() == 1)
                sys::renameFile(saved + "/" + files[0], kept + "/" + files[0]);
            sys::removeTree(saved);
            settingsWith("replace=0\ndlcingamefile=0\n");
            outcome = doTicked(console, 0, "dlcfile_installed_off");
            report("and with it off, three, each holding one of them",
                   wentThrough(outcome) && savedHeld() == std::vector<std::vector<u64>>{{game}, {dlc}, {smallDlc}}, outcome + "; " + savedSaid());
        }

        // An installed DLC that's damaged is named as the DLC, without a file name, as it isn't in a file.
        sys::removeTree(consoleHere);
        sys::removeTree(saved);
        {
            SimConsole console(consoleHere, keyset);
            console.install({from + "/base.nsp", from + "/update.nsp", from + "/dlc.nsp"}, Storage::Sd);
            const InstalledContent* largest = nullptr;
            InstalledGame installed = console.describe(game);
            for (auto& meta : installed.metas) {
                for (auto& content : meta.contents) {
                    if (meta.key.type == Cnmt::MetaAddOnContent && (!largest || content.size > largest->size))
                        largest = &content;
                }
            }
            if (largest) {
                std::string path = console.contentPath(Storage::Sd, largest->id);
                Bytes bytes = readWhole(path);
                bytes[bytes.size() - 30000] ^= 0xFF;
                sys::File out(path, sys::File::Mode::Create);
                out.write(0, bytes.data(), bytes.size());
            }
            settingsWith("replace=0\n");
            outcome = doTicked(console, 0, "dlcfile_installed_damaged");
            std::string itsDlc = format("Its DLC %016llX", (unsigned long long)dlc);
            report("an installed DLC that's damaged is named, with no file name, and nothing is left saved",
                   largest && outcome.find(itsDlc + ": ") != std::string::npos && outcome.find(itsDlc + " in '") == std::string::npos && savedFiles().empty(),
                   outcome + "; " + savedSaid());
        }

        // That one file installed puts the game and both DLC on a console that had none of it.
        sys::removeTree(consoleHere);
        {
            SimConsole console(consoleHere, keyset);
            settingsWith("installgamefiles=1\nfilesfolder=" + kept + "\n");
            outcome = doTicked(console, 1, "dlcfile_installed_again");
            int bases = 0, dlcs = 0;
            for (u64 id : console.listApplications()) {
                for (auto& meta : console.describe(id).metas) {
                    bases += meta.key.type == Cnmt::MetaApplication ? 1 : 0;
                    dlcs += meta.key.type == Cnmt::MetaAddOnContent ? 1 : 0;
                }
            }
            std::string problem = console.checkConsistent(keyset);
            report("that file installed puts the game and both DLC on the console, whole", wentThrough(outcome) && bases == 1 && dlcs == 2 && problem.empty(),
                   outcome + format("; %d games, %d DLC; ", bases, dlcs) + problem);
        }

        // The setting is with the others that rarely need changing, and is saved.
        settingsWith("");
        {
            SimConsole console(consoleHere, keyset);
            HostPlatform platform(root, keys, &console);
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            openSettings(person, app);
            person.settle();
            std::string before = optionValue(app, "Game and DLC in one file");
            if (before != "(no such option)") {
                goToOption(person, app, "Game and DLC in one file");
                person.press(ButtonA);
                person.settle();
                person.shot("dlcfile_setting");
            }
            report("Game and DLC in one file is in the settings, on to begin with, and turned off it's saved",
                   before == "On" && optionValue(app, "Game and DLC in one file") == "Off" && readText(root + "/data/settings.txt").find("dlcingamefile=0") != std::string::npos, before);
        }

        sys::removeTree(games);
        sys::removeTree(kept);
        sys::removeTree(consoleHere);
        startOver(root);
    };

    // A solid NSZ with its update, as big games come: read out of order, its streams are kept aside, in memory as far as there's room and the
    // rest in a temporary copy on the SD card. The question says so, and how much, unless all of it fits in memory; the room it needs counts
    // the copy; and once it's done, no copy is left. Its streams are kept here from the first time back, as a big game's are.
    auto solidCopyIsSaid = [&] {
        const char* solid = getenv("HBP_TEST_SOLID_FILES");
        if (!solid || !*solid) {
            printf("  (no solid NSZs to try: HBP_TEST_SOLID_FILES isn't set)\n");
            return;
        }
        wipeData(root);
        std::string games = root + "/sd/solid games", consoleHere = root + "/console_solid", temp = root + "/data/temp", logHere = root + "/data/log.txt";
        sys::removeTree(games);
        sys::removeTree(consoleHere);
        sys::makeDirectories(games);
        i64 streams = 0;
        for (const char* name : {"base.nsz", "update.nsz"}) {
            std::string from = std::string(solid) + "/" + name;
            Bytes bytes = readWhole(from);
            sys::File out(games + "/" + name, sys::File::Mode::Create);
            out.write(0, bytes.data(), bytes.size());
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(from));
            for (auto& summary : set.summarize())
                streams += summary.solidBytes;
        }
        report("the solid NSZs have streams packed solid", streams > 0, format("%lld", (long long)streams));
        writeText(root + "/data/settings.txt", "welcomed=1\nproblemsasked=1\nmeasureall=0\ndeleteasked=1\ninstallgamefiles=1\ndeleteinstalled=0\nminimumsaving=0\n"
                                               "checkwritten=1\nhidecompressed=0\nwritelog=1\nfrompcnetwork=0\nfrompccable=0\nfilesfolder=" + games + "\n");
        // What's set for the tests is put back as it was afterwards.
        auto setFor = [](const char* name, const char* value) {
            const char* was = getenv(name);
            std::string kept = was ? was : "";
            _putenv_s(name, value);
            return kept;
        };
        auto copiesLeft = [&] {
            std::string left;
            if (sys::isDirectory(temp)) {
                for (auto& entry : sys::list(temp))
                    left += (left.empty() ? "" : ", ") + entry.name;
            }
            return left;
        };

        // Opens the app on the list of game files, with this much free on the SD card, and asks to install what's ticked: the question,
        // and said yes to, what came of it.
        struct Asked {
            std::string question, outcome;
            i64 output = 0;
            size_t installed = 0;
            u32 version = 0;
        };
        auto ask = [&](const std::string& shot, i64 sdFree, bool install) {
            Asked asked;
            auto console = std::make_unique<SimConsole>(consoleHere, keyset);
            console->setFree(Storage::Sd, sdFree);
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToTab(person, app, 1);
            person.waitUntilIdle();
            person.settle();
            for (auto& row : app.snapshot().all) {
                if (row.tab == 1 && row.hasUpdate)
                    asked.output = row.afterBytes + row.marginBytes;
            }
            person.press(ButtonX);
            person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 60);
            asked.question = app.snapshot().dialog + ": " + app.snapshot().dialogText;
            person.shot(shot);
            bool going = app.snapshot().dialog.rfind("Install", 0) == 0;
            person.press(install && going ? ButtonA : ButtonB);
            if (install && going) {
                person.waitUntilIdle();
                person.shot(shot + "_done");
                asked.outcome = app.snapshot().resultsTitle;
                for (auto& line : app.snapshot().results)
                    asked.outcome += " | " + line;
                if (app.snapshot().screen == "Results")
                    person.press(ButtonA);
                for (u64 id : console->listApplications()) {
                    asked.installed++;
                    for (auto& meta : console->describe(id).metas)
                        asked.version = std::max(asked.version, meta.key.version);
                }
                healthy(*console, shot);
            }
            person.settle();
            return asked;
        };

        // With no memory to keep it in, as from the album: said, with how much.
        std::string said = format("can need a temporary copy of up to %s on the SD card", formatSize(streams).c_str());
        Asked roomy = ask("solid_question", -1, false);
        report("a solid NSZ with its update, too much for memory: the question says it can need a temporary copy on the SD card, and how much",
               roomy.question.rfind("Install", 0) == 0 && roomy.question.find("is packed solid") != std::string::npos && roomy.question.find(said) != std::string::npos,
               roomy.question);
        report("and what it comes to was found", roomy.output > 0, format("%lld", (long long)roomy.output));
        // Room for the game, and not for the copy as well.
        i64 needed = roomy.output + app::RoomToSpare + streams;
        Asked tight = ask("solid_no_room_for_copy", needed - streams / 2, false);
        report("without room for the copy as well, that's said before anything starts, and that it's for the copy",
               tight.question.rfind("There isn't enough room", 0) == 0 && tight.question.find("the SD card") != std::string::npos &&
                   tight.question.find("room for a temporary copy") != std::string::npos,
               tight.question);
        Asked enough = ask("solid_room_for_copy", needed + (1 << 20), false);
        report("with room for both, it goes ahead", enough.question.rfind("Install", 0) == 0, enough.question);
        // All of it fits in memory: nothing to say.
        std::string memoryWas = setFor("HBP_TEST_SPARE_MEMORY", "4000000000");
        Asked inMemory = ask("solid_fits_in_memory", -1, false);
        report("one that fits in memory isn't said to need a copy", inMemory.question.rfind("Install", 0) == 0 && inMemory.question.find("packed solid") == std::string::npos,
               inMemory.question);

        // Installed, with its streams kept from the first time back: in memory, with no file at all.
        std::string keptWas = setFor("HBP_TEST_MOST_KEPT_SECTION", "1"), rewindsWas = setFor("HBP_TEST_REWINDS_BEFORE_KEEPING", "0");
        sys::removeFile(logHere);
        Asked memory = ask("solid_installed_in_memory", -1, true);
        std::string log = readText(logHere);
        report("installed, kept in memory", memory.outcome.rfind("Finished |", 0) == 0 && memory.installed == 1 && memory.version == 65536 &&
                                                 log.find("kept in memory") != std::string::npos && log.find("temporary copy") == std::string::npos,
               memory.outcome + format("; %d installed, v%u", (int)memory.installed, memory.version));
        report("with no copy on the card", copiesLeft().empty(), copiesLeft());

        // And with no memory for it: a temporary copy on the card, gone once it's done.
        _putenv_s("HBP_TEST_SPARE_MEMORY", memoryWas.c_str());
        sys::removeTree(consoleHere);
        sys::removeFile(logHere);
        Asked file = ask("solid_installed_with_copy", -1, true);
        log = readText(logHere);
        report("installed again, going through a temporary copy on the card", file.outcome.rfind("Finished |", 0) == 0 && file.installed == 1 && file.version == 65536 &&
                                                                              log.find("temporary copy of up to") != std::string::npos,
               file.outcome + format("; %d installed, v%u", (int)file.installed, file.version));
        report("and the copy is gone once it's done", copiesLeft().empty(), copiesLeft());
        _putenv_s("HBP_TEST_MOST_KEPT_SECTION", keptWas.c_str());
        _putenv_s("HBP_TEST_REWINDS_BEFORE_KEEPING", rewindsWas.c_str());

        sys::removeTree(consoleHere);
        sys::removeTree(games);
    };

    // The log is closed and opened again once a second, and a card can turn opening it down now and then. What's said meanwhile is kept, and
    // goes in once the card lets it, a second later or so. It used to stop the log for good, so all a tester's log held was the app opening.
    auto logRefused = [&] {
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        std::string logHere = root + "/data/log.txt";
        auto count = [](const std::string& text, const std::string& what) {
            int found = 0;
            for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1))
                found++;
            return found;
        };
        // Over a second after the last thing said, the log is closed and opened again with the next, so what comes after that waits on the card.
        auto lookAgain = [&](int tab) {
            goToTab(person, app, tab);
            person.waitUntilIdle();
            person.waitFor([] { return false; }, 1.2);
            chooseInMenu(person, app, "Refresh the list");
            person.waitFor([&] { return app.snapshot().scanning; }, 10);
            person.waitUntilIdle();
        };
        const std::string filesLine = "game files and folders on this list", installedLine = "games installed, ";
        // Read past what turns the app down, as that's every file of this process the tests name.
        auto readPast = [](const std::string& path) {
            std::string text;
            FILE* file = nullptr;
            if (_wfopen_s(&file, std::filesystem::u8path(path).wstring().c_str(), L"rb") != 0 || !file)
                return text;
            char buffer[65536];
            for (size_t got; (got = fread(buffer, 1, sizeof buffer, file)) > 0;)
                text.append(buffer, got);
            fclose(file);
            return text;
        };
        std::string before = readText(logHere);
        report("the log is being kept", !before.empty(), format("%d bytes", (int)before.size()));
        // Turned down for a few seconds. What's said first still goes in, as the log is open then: it's closing it and opening it again
        // afterwards that's turned down. Then Game files is looked through again, which says so in the log.
        _putenv_s("HBP_TEST_REFUSE", logHere.c_str());
        lookAgain(0);
        lookAgain(1);
        person.waitFor([] { return false; }, 2.5);
        std::string refused = readPast(logHere);
        _putenv_s("HBP_TEST_REFUSE", "");
        // Let through, and once it's been asked for again, a few seconds on, Installed games looked through again.
        person.waitFor([] { return false; }, 5.5);
        lookAgain(0);
        person.waitFor([] { return false; }, 1.5);
        std::string after = readText(logHere);
        report("while the card turns the log down, what's said isn't written", count(refused, filesLine) == count(before, filesLine),
               format("%d then %d", count(before, filesLine), count(refused, filesLine)));
        report("what's said meanwhile goes in once the card lets it", count(after, filesLine) > count(before, filesLine),
               format("%d then %d", count(before, filesLine), count(after, filesLine)));
        report("and the log goes on after", count(after, installedLine) > count(refused, installedLine) && after.size() > refused.size(),
               format("%d then %d, %d bytes then %d", count(refused, installedLine), count(after, installedLine), (int)refused.size(), (int)after.size()));
    };

    // A tester's console turned log.txt down each time the app opened, so the log was log-other.txt, which nobody knew to look for. It gets the
    // usual name once the system lets it, and an old log.txt the system held on to as the app opened goes then.
    auto logNamed = [&] {
        std::string logHere = root + "/data/log.txt", otherHere = root + "/data/log-other.txt";
        // Past what turns the app down, and shared, as the app has the log open.
        auto rawText = [](const std::string& path) {
            std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
            if (!file)
                return std::string("(not there)");
            return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        };
        auto console = freshConsole();
        writeText(logHere, "An old log\n");
        _putenv_s("HBP_TEST_REFUSE", ("!" + logHere).c_str());
        // Every game has a newer version, as far as the console knows, which isn't worth a line for a game not known to be compressed, as none
        // is the first time the app opens.
        for (u64 application : console->listApplications())
            console->versionList[application] = {0x7FFF0000, 0};
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        auto moreToSay = [&] {
            person.waitFor([] { return false; }, 1.2);
            chooseInMenu(person, app, "Refresh the list");
            person.waitFor([&] { return app.snapshot().scanning; }, 10);
            person.waitUntilIdle();
        };
        moreToSay();
        // The console says it couldn't create it, and the made-up one that it couldn't open it, both with the system's code.
        const std::string turnedDown = "'" + logHere + "' (2001-0106)", installedLine = "games installed, ", renamed = "Renamed log-other.txt to log.txt";
        std::string other = rawText(otherHere), old = rawText(logHere);
        report("with log.txt turned down as the app opens, the log is kept as log-other.txt, which says why",
               other.find(turnedDown) != std::string::npos && other.find(installedLine) != std::string::npos, other.substr(0, 300));
        report("and log.txt is left as it was meanwhile", old == "An old log\n", old.substr(0, 100));
        _putenv_s("HBP_TEST_REFUSE", "");
        // It's asked for every few seconds, as each time it's turned down is a refused request.
        person.waitFor([] { return false; }, 5.5);
        moreToSay();
        moreToSay();
        std::string named = rawText(logHere);
        report("once the system lets it, the log is log.txt, with everything since the app opened in it",
               named.find(turnedDown) != std::string::npos && named.find(renamed) != std::string::npos && named.find("An old log") == std::string::npos &&
               named.find(installedLine) != std::string::npos, named.substr(0, 300));
        report("and log-other.txt is gone", !std::filesystem::exists(std::filesystem::u8path(otherHere)));
        size_t at = named.find(renamed);
        report("and the log goes on in it", at != std::string::npos && named.find(installedLine, at) != std::string::npos,
               at == std::string::npos ? std::string("not renamed") : named.substr(at, 300));
        report("a game not known to be compressed, which the console only offers a newer version of, isn't named in the log",
               named.find("offers a newer version") == std::string::npos && named.find(installedLine) != std::string::npos);
    };

    // Keep a log turned off and on again while the log is log-other.txt, as the system turned log.txt down when the app opened, and lets it now:
    // the new log is log.txt, which giving log-other.txt that name later used to delete, losing the rest of the log.
    auto logToggled = [&] {
        std::string logHere = root + "/data/log.txt";
        auto console = freshConsole();
        _putenv_s("HBP_TEST_REFUSE", ("!" + logHere).c_str());
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        _putenv_s("HBP_TEST_REFUSE", "");
        goToOption(person, app, "Keep a log");
        person.press(ButtonA);
        person.settle();
        std::string off = optionValue(app, "Keep a log");
        person.press(ButtonA);
        person.settle();
        std::string on = optionValue(app, "Keep a log");
        while (app.snapshot().screen != "Main")
            person.press(ButtonB);
        person.settle();
        // Past when log-other.txt would have been given its name, then more said.
        person.waitFor([] { return false; }, 6.5);
        chooseInMenu(person, app, "Refresh the list");
        person.waitFor([&] { return app.snapshot().scanning; }, 10);
        person.waitUntilIdle();
        person.waitFor([] { return false; }, 1.5);
        chooseInMenu(person, app, "Refresh the list");
        person.waitFor([&] { return app.snapshot().scanning; }, 10);
        person.waitUntilIdle();
        std::ifstream file(std::filesystem::u8path(logHere), std::ios::binary);
        std::string log = file ? std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()) : std::string("(not there)");
        int installed = 0;
        for (size_t at = log.find("games installed, "); at != std::string::npos; at = log.find("games installed, ", at + 1))
            installed++;
        report("Keep a log turned off and on while the log was log-other.txt, the new log.txt stays, and goes on", off != on && installed >= 2,
               off + " then " + on + ", " + format("%d refreshes in it: ", installed) + log.substr(0, 200));
    };

    // The list says what went wrong in a few plain words, worked out from the failure, which is in its details. Fed with failures as
    // testers' logs and these tests have had them.
    auto plainReasons = [&] {
        const std::pair<const char*, const char*> seen[] = {
            {"f110b369185b0cf03a501931939e5dd1.nca section 1: hash check failed at offset 0x12e0000. The file is damaged. Dump or download it again, "
             "and check that an update is for this game's version.", "Damaged: dump or download it again."},
            {"Couldn't create 'sdmc:/config/hacBrewPackLZ/analysis.txt' (2001-0123)", "The SD card stopped answering. Open the app again."},
            {"Couldn't create 'sdmc:/switch/hacBrewPackLZ/journal/journal.new' (2001-0106)", "The SD card was busy. Try again."},
            {"Couldn't write 'sdmc:/hacBrewPackLZ/x.nsp' (2002-4517). The system says the file system it was writing to is damaged, so nothing more "
             "was written. Back up the SD card on a PC, check it there, and test it with H2testw or F3 before using it again.",
             "The SD card's file system is damaged. Check it on a PC."},
            {"What was written to the SD card doesn't read back the same (0123abcd). The card may be failing", "The SD card may be failing. Check it on a PC."},
            {"The new copy reads data/a.bin differently from the original, so the original stays", "The new copy didn't come out right, so the original stays."},
            {"'Game.nsp' isn't a readable NCA3. Your keys file may not be this console's, or may be damaged: dump it again with Lockpick_RCM.",
             "Your keys can't open this game."},
            {"key_area_key_application_10 is not in your keys file. Update the console's system version, then dump your keys again with Lockpick_RCM.",
             "Your keys are too old for this game."},
            {"0100A00019C1A000 needs a title key, and its ticket isn't there", "This game's ticket is missing."},
            {"8d5340a7ab803fef0a84f3cb8da468fb.nca section 1: hash check failed at offset 0x20c96c000. The file is damaged, or this update doesn't belong "
             "to this base game. If the game was compressed with an earlier update in it before version 1.5, or its developer compressed it, the "
             "original game is needed for this update.", "Damaged, or its update is for another version: check both."},
            {"Program: the update needs a part of the original game that the compressed game doesn't have", "This update needs the original game."},
            {"Program: the update needs a part of the original game that the repacked game doesn't have", "This update needs the original game."},
            // An update that can't be made part of an installed game (checkUpdateFits), as it says it now.
            {"This compressed game has update v65536 in it already, so it doesn't have all of the original game the new update needs, and the game "
             "won't start. Remove the new update, and put its file on Game files with update v65536's file or the original game",
             "This update needs the earlier update, or the original game."},
            {"The installed repacked game has update v65536 in it already, so it doesn't have all of the original game this update needs. Put update "
             "v65536's file on Game files too, or the original game", "This update needs the earlier update, or the original game."},
            {"This update needs parts of the original game that the repacked game doesn't have, so the game won't start. Remove the update, and put its "
             "file on Game files with the original game", "This update needs the original game."},
            {"This update needs parts of the original game that the compressed game doesn't have, so the game won't start. Remove the update, and put "
             "its file on Game files with the original game", "This update needs the original game."},
            {"The repacked game that's installed doesn't have all of the original game this update needs. Put the original game on Game files with it",
             "This update needs the original game."},
            {"This update can't be added to the game that's installed: the game is damaged, or the update is for another version of it",
             "The game is damaged, or this update is for another version."},
            // As errors saved by a version before still say them.
            {"Program: the update needs a part of the original game that the compressed copy doesn't have", "This update needs the original game."},
            {"This copy has update v65536 in it already, and the new update doesn't fit it by itself, so the game won't start. Remove the new update, "
             "and put its file on Game files with update v65536's file or the original game", "This update needs the earlier update, or the original game."},
            {"This update doesn't fit the compressed copy that's installed. Put the original game on Game files with it", "This update needs the original game."},
            {"This update doesn't fit the game that's installed, so it can't be made part of it", "The game is damaged, or this update is for another version."},
            {"There isn't enough room on the SD card: this needs 1.20 GB more, and 300.00 MB is free", "There isn't enough free space."},
            {"There isn't enough memory for this one here. Hold R while starting a game to open this app with all the memory there is",
             "There isn't enough memory."},
            {"The PC can't be found any more. Is hacbrewpacklz send still running on it?", "The PC it was on stopped sharing it."},
            // A DLC that stops its game's file being made, unless what's wrong is more particular than that.
            {"Its DLC 0100ABCD12341001 in 'dlc.nsp': f110b369185b0cf03a501931939e5dd1.nca section 0: hash check failed at offset 0x150000. The file is "
             "damaged. Dump or download it again, and check that an update is for this game's version.",
             "A DLC is damaged: get it again, or turn off Game and DLC in one file."},
            {"Its DLC 0100ABCD12341001: 0100ABCD12341001 needs a title key, and its ticket isn't there", "This game's ticket is missing."},
            {"Its DLC 0100ABCD12341001 in 'dlc.nsp': Couldn't read 'sdmc:/games/dlc.nsp' (2002-2001)", "The SD card couldn't be reached. Check it's in properly."},
            {"Something unexpected went wrong: bad cast. Please report it, from + and then Help.", "Something went wrong. Its details say what."},
            {"It didn't work for some reason", "Something went wrong. Its details say what."},
            {"Couldn't open 'sdmc:/games/Game.nsp' (2002-0001)", "A file it needed isn't there any more."},
            {"Couldn't delete 'sdmc:/games/Game.nsp' (2002-0007)", "A file it needed was in use. Try again."},
            {"Couldn't write to 'sdmc:/switch/hacBrewPackLZ/x.nsp': the card may be full (2002-0030)", "There isn't enough free space."},
            {"Couldn't mount the new copy (2002-1004)", "This game's ticket is missing."},
            {"Couldn't read 'sdmc:/games/Game.nsp' (2002-2001)", "The SD card couldn't be reached. Check it's in properly."},
            {"Couldn't read the game card's game (2002-2520)", "The game card couldn't be read. Check it's in properly."},
            {"Couldn't open a game's record (2002-3201)", "There isn't enough memory."},
            {"Couldn't write the new copy (2002-3501)", "The console's own storage couldn't be reached. Restart it."},
            {"Couldn't read 'sdmc:/games/Game.nsp' (2002-4687)", "The SD card's file system is damaged. Check it on a PC."},
            {"Couldn't read the installed game (2002-4517)", "Damaged: dump or download it again."},
            {"Couldn't rename 'sdmc:/games/a.nsp' (2002-0002)", "Something went wrong. Its details say what."},
            {"This console can't start compressed games yet, so nothing was changed. It needs sigpatches, and system version 12.0.0 or newer. The firmware reads /random.bin differently from what was written, so the original stays", "This console can't start compressed games yet."},
            {"This console can't start compressed games yet, so nothing was changed. It needs sigpatches, and system version 12.0.0 or newer. Couldn't mount the test copy (2002-4604)", "This console can't start compressed games yet."},
            {"This console's firmware won't read compressed games, so nothing was changed. It needs firmware 12.0.0 or newer and sigpatches. Couldn't mount the test copy (2002-4604)", "This console can't start compressed games yet."},
        };
        int wrong = 0;
        std::string first;
        for (auto& failure : seen) {
            std::string said = app::plainReason(failure.first);
            if (said != failure.second) {
                wrong++;
                if (first.empty())
                    first = std::string(failure.first).substr(0, 60) + "... gave " + said;
            }
        }
        report("each failure comes out in the plain words it should", wrong == 0, format("%d wrong, first: ", wrong) + first);
        // And each fits the line under a game's name, 500 wide at 14.5 as app_draw.cpp draws it, measured with the font that stands in for
        // the console's here. The console's own isn't to be had on the PC, so this is close rather than exact.
        {
            Bytes face = readWhole("C:/Windows/Fonts/segoeui.ttf");
            Font font;
            font.addFace(face.data(), face.size());
            float most = 0;
            std::string widest;
            for (auto& failure : seen) {
                float width = font.measure(failure.second, 14.5f);
                if (width > most) {
                    most = width;
                    widest = failure.second;
                }
            }
            printf("  widest plain reason: %.0f of 500, \"%s\"\n", most, widest.c_str());
            report("each plain reason fits the line under a game's name", font.hasFaces() && most <= 500, format("%.0f wide: ", most) + widest);
        }
        // An error that already says what to do gets nothing more on top; one about a part a compressed or repacked game doesn't have gets what to
        // do, in this wording or the one before.
        std::string saysWhat = "This update needs parts of the original game that the compressed game doesn't have, so the game won't start. Remove the update, "
                               "and put its file on Game files with the original game";
        report("an error that already says what to do gets no next step on top", app::withNextStep(saysWhat) == saysWhat, app::withNextStep(saysWhat));
        std::string part = "Program: the update needs a part of the original game that the repacked game doesn't have";
        std::string partBefore = "Program: the update needs a part of the original game that the compressed copy doesn't have";
        report("one about a part a compressed or repacked game doesn't have gets what to do, in either wording",
            app::withNextStep(part).find("original game is needed") != std::string::npos && app::withNextStep(partBefore).find("original game is needed") != std::string::npos,
            app::withNextStep(part));
    };

    // On the SD card, game files are found however far down they are, but for the folders games aren't kept in: the system's, hidden ones, and
    // apps' own things deep in switch, which have tests of their own. For a while it went no further than three levels down, outside a folder at
    // the top where game files had been found, which was more than anyone could be expected to know about where to put them.
    // In a function of its own, so the apps in it aren't on the stack for the whole of the test, which they'd overflow.
    auto walkLevels = [&] {
        if (nsps.size() < 4)
            return;
        bool gamesAside = sys::isDirectory(root + "/sd/games");
        if (gamesAside)
            sys::renameFile(root + "/sd/games", root + "/games kept aside");
        if (sys::isDirectory(root + "/output"))
            sys::renameFile(root + "/output", root + "/output kept aside");
        std::string tree = root + "/sd/Emulator";
        sys::removeTree(tree);
        auto put = [&](const std::string& path) {
            sys::makeDirectories(path.substr(0, path.rfind('/')));
            Bytes bytes = readWhole(nsps[2]);
            sys::File out(path, sys::File::Mode::Create);
            out.write(0, bytes.data(), bytes.size());
        };
        wipeData(root);
        writeText(root + "/data/settings.txt", "welcomed=1\nproblemsasked=1\nmeasureall=0\ndeleteasked=1\ninstallgamefiles=0\nminimumsaving=0\ncheckwritten=0\n"
                                               "hidecompressed=0\nwritelog=1\nfrompcnetwork=0\nfrompccable=0\n");
        sys::removeTree(root + "/console15");
        SimConsole console(root + "/console15", keyset);
        // The app opened and left to go through everything, then what its log says the walk found and left out.
        auto walk = [&](int& gameFiles, int& leftOut) {
            {
                HostPlatform platform(root, keys, &console);
                App app(platform);
                app.start();
                Person person(app, shots);
                person.waitUntilIdle();
                goToTab(person, app, 1);
                person.waitUntilIdle();
            }
            std::string log = readText(root + "/data/log.txt");
            gameFiles = leftOut = -1;
            size_t at = log.rfind("Went through ");
            if (at == std::string::npos)
                return std::string("not in the log");
            std::string line = log.substr(at, log.find('\n', at) - at);
            size_t left = line.find(" left out "), found = line.find(" and found ");
            if (left != std::string::npos)
                leftOut = atoi(line.c_str() + left + 10);
            if (found != std::string::npos)
                gameFiles = atoi(line.c_str() + found + 11);
            return line;
        };
        int none = 0, noneLeft = 0, files = 0, left = 0;
        walk(none, noneLeft);
        put(tree + "/a/b/c/d/Deep Game.nsp");
        std::string said = walk(files, left);
        report("on the card, a game file five folders down, with nothing else in its folder at the top, is found", none >= 0 && files == none + 1 && left == noneLeft,
               format("%d then %d game files, %d then %d left out: ", none, files, noneLeft, left) + said);
        sys::removeTree(tree);
        // Where NSPs are saved, four folders down under a folder at the top with nothing else in it, which is how a copy saved there is found.
        std::string kept = root + "/sd/Kept/a/b/c/out";
        writeText(root + "/data/settings.txt", "welcomed=1\nproblemsasked=1\nmeasureall=0\ndeleteasked=1\ninstallgamefiles=0\nminimumsaving=0\ncheckwritten=0\n"
                                               "hidecompressed=0\nwritelog=1\nfrompcnetwork=0\nfrompccable=0\nnspfolder=" + kept + "\n");
        put(kept + "/Saved Here [LZ4].nsp");
        said = walk(files, left);
        report("so is one where NSPs are saved, however far down that is", files == none + 1, format("%d then %d game files: ", none, files) + said);
        sys::removeTree(root + "/sd/Kept");
        if (gamesAside)
            sys::renameFile(root + "/games kept aside", root + "/sd/games");
        sys::removeTree(root + "/output");
        if (sys::isDirectory(root + "/output kept aside"))
            sys::renameFile(root + "/output kept aside", root + "/output");
        sys::removeTree(root + "/console15");
        startOver(root);
    };

    // Opened and closed with nothing new worked out, what's been worked out isn't written again. An answer from the version before that waits
    // for its game to come up, or one that wanted keys for a game that isn't there, used to have both copies written each time the app was
    // opened and closed, as loading them counted as a change.
    auto answersKeptAsTheyAre = [&] {
        auto console = freshConsole();
        std::string answers = root + "/data/analysis.txt", backup = root + "/config/analysis.txt";
        auto openAndClose = [&] {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToTab(person, app, 1);
            person.waitUntilIdle();
        };
        // What it works out the first time.
        openAndClose();
        std::string waiting = "4|0100000000000bad|0123456789abcdef|9|65536\t1000000 500000 0 0 0 0 0\n"
                              "5|0100000000000bad|fedcba9876543210|9|65536\tfailed The key area key for generation 0b is not in your keys, it is needed for x.nca\n";
        writeText(answers, waiting + readText(answers));
        auto written = [](const std::string& path) {
            std::error_code error;
            return std::filesystem::last_write_time(std::filesystem::u8path(path), error);
        };
        auto answersBefore = written(answers), backupBefore = written(backup);
        openAndClose();
        std::string log = readText(root + "/data/log.txt");
        size_t saved = log.find("analysis.txt saved");
        bool same = written(answers) == answersBefore && written(backup) == backupBefore;
        report("opened and closed with nothing new worked out, what's been worked out isn't written again", saved == std::string::npos && same,
               (saved == std::string::npos ? std::string("no save in the log") : log.substr(saved, 60)) + (same ? ", both copies as they were" : ", a copy was written"));
        std::string kept = readText(answers);
        report("and the answers waiting for their games are still there", kept.find("4|0100000000000bad|0123456789abcdef|") != std::string::npos &&
                                                                           kept.find("5|0100000000000bad|fedcba9876543210|") != std::string::npos);
        startOver(root);
    };

    // A game asked to be worked out again from Done has no answer, which puts it under To do, and it used to wait there until To do was
    // gone to: what got its first look went by the view on the screen.
    auto analyzedAgainFromDone = [&] {
        auto console = freshConsole();
        // Compressed, and the app opened again, the games are under Done for what they are, as anyone opening it later finds them.
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToTab(person, app, 0);
            person.waitUntilIdle();
            person.press(ButtonX);
            for (int i = 0; i < 4 && !app.snapshot().dialog.empty(); i++)
                person.press(ButtonA);
            person.waitUntilIdle();
            for (int i = 0; i < 4 && app.snapshot().screen == "Results"; i++)
                person.press(ButtonA);
            person.waitUntilIdle();
        }
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        goToTab(person, app, 0);
        person.waitUntilIdle();
        for (int i = 0; i < 2 && app.snapshot().views[0] != "Done"; i++)
            person.press(app.snapshot().views[0] == "To do" ? ButtonRight : ButtonLeft);
        App::Snapshot done = app.snapshot();
        report("Installed games shows Done, with a game in it", done.views[0] == "Done" && !done.rows.empty(), done.views[0]);
        if (done.views[0] != "Done" || done.rows.empty()) {
            startOver(root);
            return;
        }
        int at = (int)done.rows.size() - 1;
        std::string name = done.rows[(size_t)at].name;
        for (int i = 0; i < 40 && app.snapshot().focus != at; i++)
            person.press(app.snapshot().focus < at ? ButtonDown : ButtonUp);
        chooseInMenu(person, app, "Analyze this game again");
        person.waitUntilIdle();
        person.shot("real_analyzed_again_from_done");
        App::Snapshot after = app.snapshot();
        const App::FullRow* row = nullptr;
        for (auto& each : after.all) {
            if (each.tab == 0 && each.name == name)
                row = &each;
        }
        report("analyzed again from Done, '" + name + "' is worked out with Done still shown", row && row->hasEstimate, row ? row->state : "not on the list");
        report("and it's back under Done", row && row->visible && after.views[0] == "Done", row ? format("%s, %s", row->visible ? "listed" : "not listed", after.views[0].c_str()) : "");

        // Worked out again a second time, it comes to just what it did the first time, which both copies of what's been worked out already
        // say. (The first time can differ from what the job that compressed it kept, by a few bytes of what it freed.)
        std::string answers = root + "/data/analysis.txt", backup = root + "/config/analysis.txt";
        auto written = [](const std::string& path) {
            std::error_code error;
            return std::filesystem::last_write_time(std::filesystem::u8path(path), error);
        };
        auto answersBefore = written(answers), backupBefore = written(backup);
        std::string textBefore = readText(answers);
        std::string logPath = root + "/data/log.txt";
        size_t logMark = readText(logPath).size();
        App::Snapshot again = app.snapshot();
        for (int i = 0; i < (int)again.rows.size(); i++) {
            if (again.rows[(size_t)i].name == name)
                at = i;
        }
        for (int i = 0; i < 40 && app.snapshot().focus != at; i++)
            person.press(app.snapshot().focus < at ? ButtonDown : ButtonUp);
        chooseInMenu(person, app, "Analyze this game again");
        person.waitUntilIdle();
        bool said = person.waitFor([&] { return readText(logPath).find("analysis.txt not saved: it would come out the same as on the card", logMark) != std::string::npos; }, 5);
        // What it said about working it out and saving, for when it doesn't hold.
        std::string log = readText(logPath), lines;
        for (size_t pos = std::min(logMark, log.size()); pos < log.size();) {
            size_t end = log.find('\n', pos);
            std::string line = log.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            if (line.find("analysis.txt") != std::string::npos || line.find("Analyzing") != std::string::npos || line.find("Measuring") != std::string::npos)
                lines += "; " + line;
            pos = end == std::string::npos ? log.size() : end + 1;
        }
        bool same = written(answers) == answersBefore && written(backup) == backupBefore && readText(answers) == textBefore;
        report("worked out again the same, what's been worked out isn't written again", same, (written(answers) == answersBefore && written(backup) == backupBefore ? "a copy changed" : "a copy was written") + lines);
        report("and the log says why", said);
        bool listed = false;
        for (auto& each : app.snapshot().all)
            listed = listed || (each.tab == 0 && each.name == name && each.visible && each.hasEstimate);
        report("and it's worked out and under Done again", listed);
        startOver(root);
    };

    // Analyzed again, a game keeps its place until its new answer, and the list stays in order around it. It used to go by name until every
    // game on it had an answer again, which with Thorough analysis on could be a long while (your console, 2026-11-10).
    auto orderKeptWhenAnalyzedAgain = [&] {
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        goToTab(person, app, 0);
        person.waitUntilIdle();
        for (int i = 0; i < 2 && app.snapshot().views[0] != "Everything"; i++)
            person.press(ButtonRight);
        person.waitUntilIdle();
        auto names = [](const App::Snapshot& s) {
            std::vector<std::string> list;
            for (auto& row : s.rows)
                list.push_back(row.name);
            return list;
        };
        auto said = [](const std::vector<std::string>& list) {
            std::string text;
            for (auto& name : list)
                text += (text.empty() ? "" : ", ") + name;
            return text;
        };
        App::Snapshot before = app.snapshot();
        std::vector<std::string> order = names(before), byName = order;
        std::sort(byName.begin(), byName.end(), [](const std::string& a, const std::string& b) { return hbp::app::lessNoCase(a, b); });
        // By name would look the same if the best saving came in the same order, which would prove nothing.
        report("Everything lists the installed games best saving first, not in the order of their names", before.views[0] == "Everything" &&
               order.size() >= 2 && before.sortedBy == "best saving first" && order != byName, before.views[0] + ", " + before.sortedBy + ": " + said(order));
        if (order.size() < 2 || order == byName) {
            startOver(root);
            return;
        }
        int at = (int)order.size() / 2;
        std::string name = order[(size_t)at];
        for (int i = 0; i < 40 && app.snapshot().focus != at; i++)
            person.press(app.snapshot().focus < at ? ButtonDown : ButtonUp);
        _putenv_s("HBP_TEST_SLOW_LOOK", "3000");
        chooseInMenu(person, app, "Analyze this game again");
        bool looking = person.waitFor([&] {
            for (auto& row : app.snapshot().all) {
                if (row.tab == 0 && row.name == name && row.state == "Working")
                    return true;
            }
            return false;
        }, 10);
        App::Snapshot during = app.snapshot();
        person.shot("real_order_kept_analyzing_again");
        report("analyzed again, '" + name + "' keeps its place while it's looked at, and the rest stay in order", looking && names(during) == order,
               (looking ? "" : "not seen being looked at; ") + said(names(during)));
        report("and the list still says it's best saving first", during.sortedBy == "best saving first", during.sortedBy);
        _putenv_s("HBP_TEST_SLOW_LOOK", "");
        person.waitUntilIdle();
        App::Snapshot after = app.snapshot();
        report("with its answer again, the list is as it was", names(after) == order, said(names(after)));
        startOver(root);
    };

    // A game asked to be worked out again while another is read in full gets its first look in the middle of it, which waits and goes on
    // from where it was. Giving way, as it used to, the game being read started over.
    auto measuringWaitsForFirstLooks = [&] {
        auto console = freshConsole();
        writeText(root + "/data/settings.txt", readText(root + "/data/settings.txt") + "measureall=1\n");
        _putenv_s("HBP_TEST_SLOW_MEASURE", "4000");
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            goToTab(person, app, 0);
            bool measuring = person.waitFor([&] {
                App::Snapshot now = app.snapshot();
                return now.measuring && std::any_of(now.all.begin(), now.all.end(), [](auto& row) { return row.tab == 0 && row.state == "Working"; });
            }, 120);
            App::Snapshot s = app.snapshot();
            std::string measured, other;
            int at = -1;
            for (auto& row : s.all) {
                if (row.tab == 0 && row.state == "Working")
                    measured = row.name;
            }
            for (int i = 0; i < (int)s.rows.size() && at < 0; i++) {
                if (s.rows[(size_t)i].name != measured && s.rows[(size_t)i].hasEstimate) {
                    other = s.rows[(size_t)i].name;
                    at = i;
                }
            }
            report("with Thorough analysis a game is read in full, and another on the list has an answer", measuring && !measured.empty() && at >= 0, measured + " / " + other);
            if (measuring && !measured.empty() && at >= 0) {
                std::string logPath = root + "/data/log.txt";
                size_t mark = readText(logPath).size();
                for (int i = 0; i < 40 && app.snapshot().focus != at; i++)
                    person.press(app.snapshot().focus < at ? ButtonDown : ButtonUp);
                // How many the reading has to go through, from what the corner says ("Measuring X, 1 of 2"), and whether the other was read
                // in full already, as then it's to be read again once it's been looked at.
                auto countIn = [](const std::string& line) {
                    size_t of = line.rfind(" of ");
                    return of == std::string::npos ? -1 : atoi(line.c_str() + of + 4);
                };
                bool otherMeasured = false;
                for (auto& row : app.snapshot().all) {
                    if (row.tab == 0 && row.name == other)
                        otherMeasured = row.measured;
                }
                int countBefore = countIn(app.snapshot().analysisLine);
                _putenv_s("HBP_TEST_SLOW_LOOK", "2000");
                chooseInMenu(person, app, "Analyze this game again");
                // While its first look is taken in the middle of the reading, the corner goes on showing the reading, rather than flicking to
                // a look that takes a second (your console, 2026-11-10), and the game itself says it's being looked at.
                bool lookSeen = person.waitFor([&] {
                    for (auto& row : app.snapshot().all) {
                        if (row.tab == 0 && row.name == other && row.state == "Working")
                            return true;
                    }
                    return false;
                }, 10);
                std::string during = app.snapshot().analysisLine;
                person.shot("real_corner_while_looking");
                report("while '" + other + "' gets its first look, the corner still says '" + measured + "' is being measured",
                       lookSeen && startsWith(during, "Measuring " + measured + ","), during);
                _putenv_s("HBP_TEST_SLOW_LOOK", "");
                // Then it joins the reading, right after the game being read, and the count says so: it used to say 1 of 1 while it waited.
                bool goneOn = person.waitFor([&] { return readText(logPath).find("Measuring " + measured + " goes on", mark) != std::string::npos; }, 15);
                std::string after = app.snapshot().analysisLine;
                report("and once it's been looked at, the count takes it in", goneOn && countBefore > 0 && countIn(after) == countBefore + (otherMeasured ? 1 : 0),
                       format("%d before, %s after", countBefore, after.c_str()) + (otherMeasured ? ", measured before" : ", not measured yet"));
                person.waitUntilIdle();
                person.waitFor([&] { return readText(logPath).find(measured + " took", mark) != std::string::npos; }, 10);
                std::string log = readText(logPath);
                log = log.substr(std::min(mark, log.size()));
                size_t waits = log.find("Measuring " + measured + " waits for a first look"), looked = log.find("Analyzing " + other),
                       goesOn = log.find("Measuring " + measured + " goes on"), took = log.find(measured + " took");
                // Started over, it would say it stopped, and that it's measuring it again.
                bool again = log.find("Stopped measuring") != std::string::npos || log.find("] Measuring " + measured + "\n") != std::string::npos ||
                             log.find("] Measuring " + measured + "\r") != std::string::npos;
                std::string said;
                for (size_t pos = 0; pos < log.size();) {
                    size_t end = log.find('\n', pos);
                    std::string line = log.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                    if (line.find("Measuring") != std::string::npos || line.find("Analyzing") != std::string::npos || line.find(" took") != std::string::npos)
                        said += "; " + line;
                    pos = end == std::string::npos ? log.size() : end + 1;
                }
                report("'" + other + "' analyzed again while '" + measured + "' is read in full gets its first look in the middle of it",
                       waits != std::string::npos && looked != std::string::npos && goesOn != std::string::npos && took != std::string::npos && waits < looked &&
                           looked < goesOn && goesOn < took,
                       said);
                report("and '" + measured + "' goes on from where it was, rather than starting over", !again && took != std::string::npos, said);
                // What was asked for goes first: the game looked at is read in full next.
                size_t next = took == std::string::npos ? std::string::npos : log.find("] Measuring ", took);
                std::string nextLine = next == std::string::npos ? "" : log.substr(next + 2, log.find('\n', next) - next - 2);
                report("and '" + other + "' is read in full right after it", startsWith(nextLine, "Measuring " + other), nextLine + said);
                bool both = true;
                for (auto& row : app.snapshot().all) {
                    if (row.tab == 0 && (row.name == measured || row.name == other))
                        both = both && row.hasEstimate;
                }
                report("and both have their answers", both);
            }
        }
        _putenv_s("HBP_TEST_SLOW_MEASURE", "");
        startOver(root);
    };

    // A game read in full already, analyzed again while another is read in full, joins that reading right after it, and the count in the
    // corner takes it in. It used to wait for a reading of its own, the corner saying 1 of 1 meanwhile (your console, 2026-11-10).
    auto analyzedAgainJoinsTheReading = [&] {
        auto console = freshConsole();
        writeText(root + "/data/settings.txt", readText(root + "/data/settings.txt") + "measureall=1\n");
        _putenv_s("HBP_TEST_SLOW_MEASURE", "4000");
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            goToTab(person, app, 0);
            // One game read in full already, and another being read.
            std::string done, measured;
            bool ready = person.waitFor([&] {
                App::Snapshot now = app.snapshot();
                done.clear();
                measured.clear();
                for (auto& row : now.all) {
                    if (row.tab != 0)
                        continue;
                    if (row.measured && row.state != "Working")
                        done = row.name;
                    if (row.state == "Working" && now.measuring)
                        measured = row.name;
                }
                return !done.empty() && !measured.empty();
            }, 180);
            App::Snapshot s = app.snapshot();
            report("with Thorough analysis one game is read in full already and another is being read", ready, done + " / " + measured);
            if (ready) {
                auto countIn = [](const std::string& line) {
                    size_t of = line.rfind(" of ");
                    return of == std::string::npos ? -1 : atoi(line.c_str() + of + 4);
                };
                int countBefore = countIn(s.analysisLine);
                int at = -1;
                for (int i = 0; i < (int)s.rows.size(); i++) {
                    if (s.rows[(size_t)i].name == done)
                        at = i;
                }
                for (int i = 0; i < 40 && at >= 0 && app.snapshot().focus != at; i++)
                    person.press(app.snapshot().focus < at ? ButtonDown : ButtonUp);
                std::string logPath = root + "/data/log.txt";
                size_t mark = readText(logPath).size();
                chooseInMenu(person, app, "Analyze this game again");
                bool goneOn = person.waitFor([&] { return readText(logPath).find("Measuring " + measured + " goes on", mark) != std::string::npos; }, 20);
                std::string after = app.snapshot().analysisLine;
                report("'" + done + "', read in full already and analyzed again, joins the reading of '" + measured + "', and the count takes it in",
                       goneOn && countBefore > 0 && countIn(after) == countBefore + 1 && startsWith(after, "Measuring " + measured + ","),
                       format("%s before, %s after", s.analysisLine.c_str(), after.c_str()));
                person.waitUntilIdle();
                std::string log = readText(logPath);
                log = log.substr(std::min(mark, log.size()));
                size_t took = log.find(measured + " took");
                size_t next = took == std::string::npos ? std::string::npos : log.find("] Measuring ", took);
                std::string nextLine = next == std::string::npos ? "" : log.substr(next + 2, log.find('\n', next) - next - 2);
                report("and it's read in full right after it", startsWith(nextLine, "Measuring " + done), nextLine);
            }
        }
        _putenv_s("HBP_TEST_SLOW_MEASURE", "");
        startOver(root);
    };

    // Thorough turned off while the game being read in full waits for a first look stops the reading there, as it does at any other time,
    // rather than letting it go on once the look is done.
    auto turnedOffWhileLooking = [&] {
        auto console = freshConsole();
        writeText(root + "/data/settings.txt", readText(root + "/data/settings.txt") + "measureall=1\n");
        _putenv_s("HBP_TEST_SLOW_MEASURE", "8000");
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            goToTab(person, app, 0);
            bool measuring = person.waitFor([&] {
                App::Snapshot now = app.snapshot();
                return now.measuring && std::any_of(now.all.begin(), now.all.end(), [](auto& row) { return row.tab == 0 && row.state == "Working"; });
            }, 120);
            App::Snapshot s = app.snapshot();
            std::string measured, other;
            int at = -1;
            for (auto& row : s.all) {
                if (row.tab == 0 && row.state == "Working")
                    measured = row.name;
            }
            for (int i = 0; i < (int)s.rows.size() && at < 0; i++) {
                if (s.rows[(size_t)i].name != measured && s.rows[(size_t)i].hasEstimate) {
                    other = s.rows[(size_t)i].name;
                    at = i;
                }
            }
            report("with Thorough analysis a game is read in full, and another on the list has an answer to work out again", measuring && !measured.empty() && at >= 0,
                   measured + " / " + other);
            if (measuring && !measured.empty() && at >= 0) {
                std::string logPath = root + "/data/log.txt";
                size_t mark = readText(logPath).size();
                for (int i = 0; i < 40 && app.snapshot().focus != at; i++)
                    person.press(app.snapshot().focus < at ? ButtonDown : ButtonUp);
                _putenv_s("HBP_TEST_SLOW_LOOK", "6000");
                chooseInMenu(person, app, "Analyze this game again");
                bool waiting = person.waitFor([&] { return readText(logPath).find("Measuring " + measured + " waits for a first look", mark) != std::string::npos; }, 10);
                goToOption(person, app, "Analysis");
                person.press(ButtonRight);
                std::string chosen = optionValue(app, "Analysis");
                bool looking = readText(logPath).find("Measuring " + measured + " goes on", mark) == std::string::npos;
                _putenv_s("HBP_TEST_SLOW_LOOK", "");
                goToTab(person, app, 0);
                report("Analysis set to Quick while '" + measured + "' waits for '" + other + "' to have its first look", waiting && looking && chosen == "Quick",
                       format("waiting %d, still looking %d, ", waiting, looking) + chosen);
                bool stopped = person.waitFor([&] { return readText(logPath).find("Stopped measuring", mark) != std::string::npos && !app.snapshot().measuring; }, 15);
                bool again = person.waitFor([&] { return app.snapshot().measuring; }, 12);
                std::string log = readText(logPath);
                log = log.substr(std::min(mark, log.size()));
                std::string said;
                for (size_t pos = 0; pos < log.size();) {
                    size_t end = log.find('\n', pos);
                    std::string line = log.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                    if (line.find("Measuring") != std::string::npos || line.find("Analyzing") != std::string::npos || line.find("Stopped") != std::string::npos)
                        said += "; " + line;
                    pos = end == std::string::npos ? log.size() : end + 1;
                }
                report("the reading stops there, and nothing is read in full again", stopped && !again && log.find(measured + " took") == std::string::npos &&
                                                                                         readText(root + "/data/settings.txt").find("measureall=0") != std::string::npos,
                       format("stopped %d, started again %d", stopped, again) + said);
                bool answered = person.waitFor([&] {
                    for (auto& row : app.snapshot().all) {
                        if (row.tab == 0 && row.name == other)
                            return row.hasEstimate;
                    }
                    return false;
                }, 60);
                report("and '" + other + "' still gets its answer", answered);
            }
        }
        _putenv_s("HBP_TEST_SLOW_MEASURE", "");
        _putenv_s("HBP_TEST_SLOW_LOOK", "");
        startOver(root);
    };

    // Details and Look inside: an installed game's facts, then what it's made of, its icon as a picture, a file of another kind as its bytes
    // and on the whole screen, saving it to the SD card, finding a file by name, and a game file looked inside too.
    auto lookingInside = [&] {
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        platform.system = 17u << 26;
        // The app's output on the SD card, as on the console, where the list of game files looks.
        platform.output = root + "/sd/app output";
        // A file of a game with its own icon, on the SD card, to look inside too.
        std::string file = root + "/sd/games/look inside.nsp";
        {
            Bytes whole = readWhole(nsps[0]);
            sys::File out(file, sys::File::Mode::Create);
            out.write(0, whole.data(), whole.size());
        }
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        goToTab(person, app, 0);
        person.waitUntilIdle();
        // To the game named, on the list shown: the first whose detail says so, when there's one to go by.
        auto goToGame = [&](const std::string& name, const std::string& detail) {
            auto rows = app.snapshot().rows;
            int at = -1;
            for (int i = 0; i < (int)rows.size(); i++) {
                if (!containsNoCase(rows[(size_t)i].name, name))
                    continue;
                if (at < 0 || (!detail.empty() && containsNoCase(rows[(size_t)i].detail, detail))) {
                    at = i;
                    if (detail.empty() || containsNoCase(rows[(size_t)i].detail, detail))
                        break;
                }
            }
            for (int step = 0; at >= 0 && step < 200 && app.snapshot().focus != at; step++)
                person.press(app.snapshot().focus < at ? ButtonDown : ButtonUp);
            return at >= 0;
        };
        report("the game with an icon of its own is on the list", goToGame("ANIMAL WELL", ""));
        auto joinedUp = [](const std::vector<std::string>& lines) {
            std::string all;
            for (auto& line : lines)
                all += line + "; ";
            return all;
        };
        auto goToEntry = [&](const std::string& part) {
            auto entries = app.snapshot().insideEntries;
            for (int i = 0; i < (int)entries.size(); i++) {
                if (!containsNoCase(entries[(size_t)i], part))
                    continue;
                while (app.snapshot().insideFocus != i)
                    person.press(app.snapshot().insideFocus < i ? ButtonDown : ButtonUp);
                return true;
            }
            return false;
        };
        auto previewed = [&] { return person.waitFor([&] { return app.snapshot().insidePreviewReady; }, 120); };

        chooseInMenu(person, app, "Details");
        report("Details opens from + and This game", app.snapshot().screen == "Details", app.snapshot().screen);
        bool ready = person.waitFor([&] { return app.snapshot().insideReady; }, 120);
        App::Snapshot s = app.snapshot();
        std::string facts = joinedUp(s.details);
        report("and says what's in it, where it is, what it needs and what compressing it does", ready && s.insideProblem.empty() &&
               facts.find("What's in it: Game ID: ") != std::string::npos && facts.find("On the console: Where: ") != std::string::npos &&
               facts.find("What's wrong: System: Needs ") != std::string::npos && facts.find("yours is 17.0.0") != std::string::npos &&
               (facts.find("Compressing it: ") != std::string::npos || facts.find("Updates: ") != std::string::npos), s.insideProblem + " " + facts);
        report("with the one figure that decides", s.detailsState.size() > 4, s.detailsState);
        // The pretend console is on 17.0.0 and the game needs newer: that comes first, with what to do, and the size isn't said again.
        report("what's wrong comes first, with what to do", facts.rfind("What's wrong: System: Needs ", 0) == 0 && facts.find("Update the console to play it") != std::string::npos,
               facts.substr(0, 160));
        report("and what it saves is said once, by the figure", facts.find("Compressing it: Saves") == std::string::npos && facts.find("Takes: ") == std::string::npos, facts);
        person.shot("real_details_installed");

        person.press(ButtonA);
        report("A looks inside", app.snapshot().screen == "Inside", app.snapshot().screen);
        previewed();
        s = app.snapshot();
        std::string top = joinedUp(s.insideEntries);
        // This one keeps all its data in its program, so it has no game data of its own.
        report("its parts are folders, named for what they're for, in the same order as any game's", top.rfind("Program | Folder", 0) == 0 &&
               top.find("Icons and names | Folder") != std::string::npos && top.find("Start-up logo | Folder") != std::string::npos, top);
        report("and the one under the pointer is shown beside them, as what it's made of", s.previewKind == "Folder" && s.previewChips.size() >= 3, s.previewKind);
        person.shot("real_inside_top");

        // Its legal notices, which are text, shown with their lines numbered, the way any text in a game is.
        if (goToEntry("Legal notices |")) {
            person.press(ButtonA);
            person.waitFor([&] { return app.snapshot().insidePath == "Legal notices"; }, 30);
            auto entries = app.snapshot().insideEntries;
            bool text = false;
            for (size_t i = 0; i < entries.size() && !text; i++) {
                if (entries[i].find(" | Folder | ") != std::string::npos)
                    continue;
                goToEntry(entries[i].substr(0, entries[i].find(" | ")) + " |");
                previewed();
                text = app.snapshot().previewKind == "Text" && !app.snapshot().previewLines.empty();
            }
            report("text in a game is shown as text, its lines numbered", text, joinedUp(app.snapshot().insideEntries));
            person.shot("real_inside_text");
            person.press(ButtonB);
            person.waitFor([&] { return app.snapshot().insidePath.empty(); }, 30);
        }

        goToEntry("Icons and names |");
        previewed();
        report("a folder with a picture straight inside it shows the picture", app.snapshot().pictureW > 0, joinedUp(app.snapshot().previewChips));
        person.shot("real_inside_icons");
        person.press(ButtonA);
        person.waitFor([&] { return app.snapshot().insidePath == "Icons and names"; }, 30);
        goToEntry("icon_");
        bool picture = person.waitFor([&] {
            auto now = app.snapshot();
            return now.insidePreviewReady && now.previewKind == "Picture" && now.pictureW > 0;
        }, 60);
        report("a game's icon is shown as a picture", picture, app.snapshot().previewKind + ": " + joinedUp(app.snapshot().previewChips));
        person.shot("real_inside_icon");

        goToEntry("control.nacp");
        person.waitFor([&] { return app.snapshot().insidePreviewReady && app.snapshot().previewKind != "Picture"; }, 60);
        s = app.snapshot();
        report("a file of another kind is shown as its bytes", joinedUp(s.previewChips).find("as bytes") != std::string::npos ||
               joinedUp(s.previewChips).find("As bytes") != std::string::npos, joinedUp(s.previewChips));
        person.press(ButtonA);
        report("A shows all of it on the whole screen", app.snapshot().insideFull);
        person.shot("real_inside_bytes");
        person.press(ButtonB);
        report("and B goes back from there", !app.snapshot().insideFull && app.snapshot().screen == "Inside");

        person.press(ButtonX);
        bool saved = person.waitFor([&] { return !app.snapshot().insideSaving && !app.snapshot().insideSavedTo.empty(); }, 60);
        std::string to = app.snapshot().insideSavedTo;
        report("X saves it to the SD card, whole", saved && sys::exists(to) && sys::fileSize(to) == 0x4000, to);
        report("in a folder of its own, apart from where extracted games go", to.find("/Files from games/") != std::string::npos &&
               to.find("/extracted/") == std::string::npos, to);
        // A whole extracted game there, which anywhere else would be listed, for the list of game files to look over.
        std::string fromGames = to.substr(0, to.find("/Files from games/") + strlen("/Files from games"));
        {
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(nsps[0]));
            set.group();
            extractTitle(set.jobs.at(0), set, fromGames);
        }
        size_t inGame = to.find("/Icons and names");
        if (inGame != std::string::npos)
            sys::removeTree(to.substr(0, inGame));

        platform.typed = "NACP";
        person.press(ButtonY);
        s = app.snapshot();
        report("Y finds a file by name, from anywhere in the game", s.insidePath == "found: NACP" && !s.insideEntries.empty() &&
               s.insideEntries[0].rfind("control.nacp", 0) == 0, s.insidePath + ": " + joinedUp(s.insideEntries));
        person.press(ButtonB);
        report("B goes back from what was found", app.snapshot().insidePath == "Icons and names", app.snapshot().insidePath);
        person.press(ButtonB);
        person.press(ButtonB);
        report("then up to the top, then to the details it was opened from", app.snapshot().screen == "Details", app.snapshot().screen);
        person.press(ButtonB);
        report("and from there to the list", app.snapshot().screen == "Main", app.snapshot().screen);

        goToTab(person, app, 1);
        person.waitUntilIdle();
        // Looked through again, what's in Files from games isn't listed, even shaped like an extracted game.
        chooseInMenu(person, app, "Refresh the list");
        person.waitUntilIdle();
        std::string fromThere;
        for (auto& row : app.snapshot().all) {
            if (row.mainFile.find("Files from games") != std::string::npos || row.extracted.find("Files from games") != std::string::npos)
                fromThere += row.name + "; ";
        }
        report("what Look inside saved is never listed as a game, whatever it looks like", fromThere.empty(), fromThere);
        sys::removeTree(fromGames);
        report("its file is on the list of game files", goToGame("ANIMAL WELL", "look inside"));
        chooseInMenu(person, app, "Look inside");
        ready = person.waitFor([&] { return app.snapshot().insideReady; }, 120);
        previewed();
        s = app.snapshot();
        top = joinedUp(s.insideEntries);
        report("a game file opens to look inside too, its parts in the same order", ready && s.insideProblem.empty() &&
               top.rfind("Program | Folder", 0) == 0, s.insideProblem + " " + top);
        person.shot("real_inside_file");
        person.press(ButtonB);
        report("and B goes straight back to the list it was opened from", app.snapshot().screen == "Main", app.snapshot().screen);
        healthy(*console, "after looking inside games");
        sys::removeFile(file);
        sys::removeTree(platform.output);
    };

    // A game card: its game is on the list of game files, said to be on the game card, and installs compressed beside it, while the card's
    // own copy and its records stay as they were. The Installed games list says where to install it from.
    auto gameCardTest = [&] {
        sys::renameFile(root + "/sd/games", root + "/games kept aside");
        // Copies made by the tests before are of the same game, and would be listed as it, from the SD card, which comes before the card.
        if (sys::isDirectory(root + "/output"))
            sys::renameFile(root + "/output", root + "/output kept aside");
        sys::removeTree(root + "/console13");
        sys::makeDirectories(root + "/sd/games");
        wipeData(root);
        writeText(root + "/data/settings.txt", "welcomed=1\nproblemsasked=1\nmeasureall=0\ndeleteasked=1\nminimumsaving=0\nwritelog=1\nfrompcnetwork=0\nfrompccable=0\n");
        SimConsole console(root + "/console13", keyset);
        console.install({nsps[2]}, Storage::GameCard);
        appearance(console);
        u64 game = console.listApplications().at(0);
        auto onCard = [&] {
            std::vector<std::pair<MetaKey, Storage>> card;
            for (auto& record : console.records(game)) {
                if (record.second == Storage::GameCard)
                    card.push_back(record);
            }
            return card;
        };
        auto cardBefore = onCard();
        // The app is closed before the games are put back, as a folder with anything open in it can't be renamed.
        [&] {
            HostPlatform platform(root, keys, &console);
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            std::string installedSays;
            for (auto& row : app.snapshot().all)
                installedSays += row.tab == 0 ? row.name + ": " + row.second + "; " : "";
            report("on Installed games, the game card's game says where to install it from", installedSays.find("install it from Game files") != std::string::npos, installedSays);
            goToTab(person, app, 1);
            person.waitUntilIdle();
            person.settle();
            int at = -1;
            std::string listed;
            auto rows = app.snapshot().rows;
            for (int i = 0; i < (int)rows.size(); i++) {
                listed += rows[(size_t)i].name + " / " + rows[(size_t)i].detail + "; ";
                if (rows[(size_t)i].detail.find("On the game card") != std::string::npos)
                    at = i;
            }
            report("the game card's game is on the list of game files, said to be on the game card", at >= 0 && rows.size() == 1, listed);
            report("and it's been analyzed", at >= 0 && rows[(size_t)at].hasEstimate, listed);
            person.shot("real_game_card");
            if (at < 0)
                return;
            chooseInMenu(person, app, "Install this game");
            bool asked = person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 60);
            report("installing it asks first", asked, app.snapshot().dialog);
            person.press(ButtonA);
            person.waitUntilIdle();
            report("and it installs", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
            bool onSd = false;
            for (auto& meta : console.describe(game).metas)
                onSd = onSd || (meta.storage == Storage::Sd && meta.key.type == Cnmt::MetaApplication);
            report("compressed, on the SD card", onSd);
            report("and the game card's records are as they were", onCard() == cardBefore && !cardBefore.empty());
            std::string problem = console.checkConsistent(keyset);
            report("everything is whole, the card's copy too", problem.empty(), problem);
            // B goes back, as everywhere, and A does what's under the pointer, of which there's nothing on the results.
            person.pressAsIs(ButtonA);
            report("on the results, A does nothing", app.snapshot().screen == "Results", app.snapshot().screen);
            person.pressAsIs(ButtonB);
            report("and B goes back to the list", app.snapshot().screen == "Main", app.snapshot().screen);
        }();
        sys::removeTree(root + "/sd/games");
        sys::renameFile(root + "/games kept aside", root + "/sd/games");
        sys::removeTree(root + "/output");
        if (sys::isDirectory(root + "/output kept aside"))
            sys::renameFile(root + "/output kept aside", root + "/output");
        sys::removeTree(root + "/console13");
        startOver(root);
    };

    if (getenv("HBP_TEST_ONLY_CARD")) {
        gameCardTest();
        printf("%d passed, %d failed\n", g_passes, g_failures);
        return g_failures > 0 ? 1 : 0;
    }

    // Done, after compressing the games: what each was, what it takes and what was freed.
    if (getenv("HBP_TEST_ONLY_DONE")) {
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        goToTab(person, app, 0);
        person.waitUntilIdle();
        person.shot("proto_todo");
        person.press(ButtonX);
        person.press(ButtonA);
        person.waitUntilIdle();
        person.press(ButtonB);
        person.waitUntilIdle();
        for (int i = 0; i < 3 && app.snapshot().views[0] != "Done"; i++)
            person.press(app.snapshot().views[0] == "Everything" ? ButtonLeft : ButtonRight);
        person.settle();
        person.shot("done");
        auto headings = app.snapshot().headings;
        report("Done says what each was, what it takes now and what was freed", headings == std::vector<std::string>{"Was", "Now", "Freed"},
               headings.empty() ? std::string("no headings") : headings[0] + ", " + (headings.size() > 1 ? headings[1] : ""));
        report("Done shows the compressed games", app.snapshot().views[0] == "Done" && !app.snapshot().rows.empty(), app.snapshot().views[0]);
        printf("%d passed, %d failed\n", g_passes, g_failures);
        return g_failures > 0 ? 1 : 0;
    }

    if (getenv("HBP_TEST_ONLY_INSIDE")) {
        lookingInside();
        printf("%d passed, %d failed\n", g_passes, g_failures);
        return g_failures > 0 ? 1 : 0;
    }

    if (getenv("HBP_TEST_ONLY_WALK")) {
        walkLevels();
        printf("%d passed, %d failed\n", g_passes, g_failures);
        return g_failures > 0 ? 1 : 0;
    }

    if (getenv("HBP_TEST_ONLY_SAVES")) {
        answersKeptAsTheyAre();
        printf("%d passed, %d failed\n", g_passes, g_failures);
        return g_failures > 0 ? 1 : 0;
    }

    if (getenv("HBP_TEST_ONLY_AGAIN")) {
        analyzedAgainFromDone();
        orderKeptWhenAnalyzedAgain();
        printf("%d passed, %d failed\n", g_passes, g_failures);
        return g_failures > 0 ? 1 : 0;
    }

    if (getenv("HBP_TEST_ONLY_PAUSE")) {
        measuringWaitsForFirstLooks();
        analyzedAgainJoinsTheReading();
        turnedOffWhileLooking();
        printf("%d passed, %d failed\n", g_passes, g_failures);
        return g_failures > 0 ? 1 : 0;
    }

    // Only that, for when that's all that needs trying.
    if (getenv("HBP_TEST_ONLY_LOG")) {
        plainReasons();
        logRefused();
        logNamed();
        logToggled();
        printf("%d passed, %d failed\n", g_passes, g_failures);
        return g_failures > 0 ? 1 : 0;
    }

    if (getenv("HBP_TEST_ONLY_GONE")) {
        goneFromConsole();
        printf("%d passed, %d failed\n", g_passes, g_failures);
        return g_failures > 0 ? 1 : 0;
    }

    if (getenv("HBP_TEST_ONLY_NAMES")) {
        savedUnderAFreeName();
        printf("%d passed, %d failed\n", g_passes, g_failures);
        return g_failures > 0 ? 1 : 0;
    }

    if (getenv("HBP_TEST_ONLY_DLCFILE")) {
        dlcInGameFile();
        printf("%d passed, %d failed\n", g_passes, g_failures);
        return g_failures > 0 ? 1 : 0;
    }

    if (getenv("HBP_TEST_ONLY_SOLID")) {
        solidCopyIsSaid();
        printf("%d passed, %d failed\n", g_passes, g_failures);
        return g_failures > 0 ? 1 : 0;
    }

    // Only that, for when that's all that needs trying.
    if (getenv("HBP_TEST_ONLY_REFUSED")) {
        refusedAtStart();
        printf("%d passed, %d failed\n", g_passes, g_failures);
        return g_failures > 0 ? 1 : 0;
    }

    // Only that, for when that's all that needs trying.
    if (getenv("HBP_TEST_ONLY_WAITING")) {
        waitingOnPc();
        printf("%d passed, %d failed\n", g_passes, g_failures);
        return g_failures > 0 ? 1 : 0;
    }

    // Only installing from a PC, for when that's all that needs trying.
    if (getenv("HBP_TEST_ONLY_PC")) {
        pcInstall();
        printf("%d passed, %d failed\n", g_passes, g_failures);
        return g_failures > 0 ? 1 : 0;
    }


    i64 totalBefore = 0;

    // The first run: the list, the analysis by itself, and compressing what's ticked.
    {
        auto console = freshConsole();
        for (u64 id : console->listApplications())
            totalBefore += console->describe(id).totalBytes();
        // The SD card's free room as a real card's is, with its figures as wide as they come, so the results' "was ... -> ... free"
        // beside its bar is drawn as on a console. It grows once the job is under way, as compressing frees room.
        console->setFree(Storage::Sd, 38LL * (1LL << 30) + 400LL * (1LL << 20));

        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitFor([&] { return app.snapshot().rows.size() > 0 || !app.busy(); });
        person.shot("real_first_look");
        bool sawAnalyzing = person.waitFor([&] { return app.snapshot().analyzing; }, 20);
        report("analysis starts by itself", sawAnalyzing);
        person.shot("real_analyzing");
        person.waitUntilIdle();
        person.shot("real_analyzed");

        App::Snapshot snapshot = app.snapshot();
        bool allKnown = !snapshot.rows.empty();
        for (auto& row : snapshot.rows)
            allKnown = allKnown && row.hasEstimate;
        report("every game has an answer", allKnown);
        report("the answers are remembered", sys::exists(root + "/data/analysis.txt"));
        report("the screen stayed awake while it worked", platform.workedAtAll);
        report("and may sleep again afterwards", !platform.working);

        person.press(ButtonX);
        report("compressing asks first", app.snapshot().dialog.find("Compress") == 0, app.snapshot().dialog);
        person.shot("real_confirm");
        person.press(ButtonA);
        report("the job screen shows", app.snapshot().screen == "Job");

        std::string seen;
        std::map<std::string, bool> shotOf;
        double start = sys::now();
        console->setFree(Storage::Sd, 53LL * (1LL << 30) + 563LL * (1LL << 20));
        std::vector<std::pair<double, double>> bar;
        std::vector<std::string> barLabels;
        while (app.busy() && sys::now() - start < 3600) {
            person.frame();
            App::Snapshot during = app.snapshot();
            bar.emplace_back(sys::now() - start, during.overall);
            barLabels.push_back(during.label);
            std::string label = during.label;
            if (!label.empty() && !shotOf[label]) {
                shotOf[label] = true;
                seen += label + "; ";
                std::string name = "real_job_" + std::to_string(shotOf.size());
                person.shot(name);
            }
            sys::sleepMs(5);
        }
        printf("  steps seen: %s\n", seen.c_str());
        {
            // However it's made up, the bar should be a fair idea of how much of the time has gone.
            double total = sys::now() - start;
            double worst = 0, longestStill = 0, stillSince = 0, last = 0;
            for (auto& point : bar) {
                if (point.second > last) {
                    last = point.second;
                    stillSince = point.first;
                }
                longestStill = std::max(longestStill, point.first - stillSince);
                worst = std::max(worst, fabs(point.second - point.first / total));
            }
            printf("  the bar was out by %.0f%% at the most, and stood still for %.1f s of %.1f s at the most\n", worst * 100, longestStill, total);
            // The last few times it was looked at, for when it didn't: how long in, how far along, and the step.
            std::string lastSeen;
            for (size_t i = bar.size() > 6 ? bar.size() - 6 : 0; i < bar.size(); i++)
                lastSeen += format("; %.2f s %.2f %s", bar[i].first, bar[i].second, barLabels[i].c_str());
            report("the bar got to the end", !bar.empty() && bar.back().second > 0.9, format("%.2f", bar.empty() ? 0.0 : bar.back().second) + lastSeen);
            report("and stayed a fair idea of how far along it was", worst < 0.45, format("out by %.0f%%", worst * 100));
        }
        person.settle();
        report("the results screen shows", app.snapshot().screen == "Results", app.snapshot().screen);
        person.shot("real_results");
        for (auto& line : app.snapshot().results)
            printf("  %s\n", line.c_str());
        person.press(ButtonA);
        person.settle();
        person.shot("real_after");
        healthy(*console, "after compressing");

        i64 totalAfter = 0;
        for (u64 id : console->listApplications())
            totalAfter += console->describe(id).totalBytes();
        printf("  %lld bytes before, %lld after\n", (long long)totalBefore, (long long)totalAfter);
        report("it took less room", totalAfter < totalBefore);
        std::string log = readText(root + "/data/log.txt");
        report("nothing unexpected in the log", log.find("Unexpected") == std::string::npos, log.substr(0, 400));
        size_t tests = 0;
        for (size_t at = log.find("files of the test copy back"); at != std::string::npos; at = log.find("files of the test copy back", at + 1))
            tests++;
        report("the console is only checked the once, however many games there are", tests == 1, format("%d times", (int)tests));
        report("nothing goes through a temporary folder", !sys::isDirectory(root + "/data/temp") || sys::list(root + "/data/temp").empty());
        report("the list is remembered for next time", sys::exists(root + "/data/list.cache"));
        size_t backed = 0;
        for (size_t at = log.find("files of "); at != std::string::npos; at = log.find("files of ", at + 1))
            backed++;
        printf("  %d lots of files read back\n", (int)backed);
        report("and the new copies were read back", backed >= 3, format("%d", (int)backed));
    }

    // Starting again: nothing to work out again, and nothing more to gain.
    {
        auto console = std::make_unique<SimConsole>(consoleFolder, keyset);
        appearance(*console);
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        report("the list is there from the first moment", !app.snapshot().rows.empty());
        Person person(app, shots);
        person.settle();
        person.waitUntilIdle();
        person.shot("real_second_start");
        report("and no names or icons had to be read again", console->controlReads == 0, format("%d", console->controlReads.load()));
        std::string log = readText(root + "/data/log.txt");
        report("the second start doesn't analyze again", log.find("Analyzing") == std::string::npos);
        App::Snapshot snapshot = app.snapshot();
        bool noneTicked = !snapshot.rows.empty();
        for (auto& row : snapshot.rows)
            noneTicked = noneTicked && !row.ticked;
        report("compressed games come up unticked", noneTicked);
        bool sayWhy = !snapshot.rows.empty(), anyCompressed = false;
        for (auto& row : snapshot.rows) {
            // A game with no game data to compress has nothing to gain, and isn't compressed either.
            sayWhy = sayWhy && (row.compressed ? row.note == "Already compressed" : row.note.find("Nothing to gain") != std::string::npos);
            anyCompressed = anyCompressed || row.compressed;
        }
        report("and say why", sayWhy && anyCompressed, snapshot.rows.empty() ? "" : snapshot.rows[0].note);

        // Doing it again by hand would come to the very same bytes, so it's said before anything starts, and nothing is touched.
        report("Y offers to tick everything", app.snapshot().tickHint == "Tick all", app.snapshot().tickHint);
        person.press(ButtonY);
        report("and does", !app.snapshot().rows.empty() && app.snapshot().rows[0].ticked && app.snapshot().toast == "Everything ticked", app.snapshot().toast);
        person.press(ButtonX);
        App::Snapshot asked = app.snapshot();
        report("compressing it all again says first there's nothing to gain", asked.dialog.rfind("Nothing to ", 0) == 0 && asked.dialogText.find("nothing to gain") != std::string::npos,
               asked.dialog + ": " + asked.dialogText);
        person.shot("real_again");
        person.press(ButtonA);
        person.waitUntilIdle();
        report("and leaves them as they are", app.snapshot().screen == "Main" && app.snapshot().resultsTitle.empty(), app.snapshot().screen + " " + app.snapshot().resultsTitle);
        healthy(*console, "after compressing it all again");
    }

    // Opened for the first time on games that are already compressed, there's nothing to work out.
    {
        sys::removeFile(root + "/data/analysis.txt");
        sys::removeFile(root + "/config/analysis.txt");
        auto console = std::make_unique<SimConsole>(consoleFolder, keyset);
        appearance(*console);
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        std::string log = readText(root + "/data/log.txt");
        report("what's already compressed doesn't get sampled", log.find("Analyzing") != std::string::npos && log.find("samples of the game data") == std::string::npos, log.substr(log.size() > 400 ? log.size() - 400 : 0));
        App::Snapshot snapshot = app.snapshot();
        bool allSay = false;
        for (auto& row : snapshot.rows) {
            if (row.compressed)
                allSay = row.hasEstimate && !row.ticked && row.note == "Already compressed";
        }
        report("and comes up unticked, with the same check as what's just been compressed", allSay);
        person.shot("real_already_compressed");
    }

    // Left to itself, what's compressed stays off the list.
    {
        std::string kept = readText(root + "/data/settings.txt");
        std::string keptElsewhere = readText(root + "/config/settings.txt");
        writeText(root + "/data/settings.txt", "welcomed=1\nproblemsasked=1\nmeasureall=0\ndeleteasked=1\ninstallgamefiles=0\nfrompcnetwork=0\nfrompccable=0\n");
        sys::removeFile(root + "/config/settings.txt");
        int games;
        {
            auto console = std::make_unique<SimConsole>(consoleFolder, keyset);
            appearance(*console);
            games = (int)console->listApplications().size();
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            App::Snapshot snapshot = app.snapshot();
            int hiddenNow = snapshot.compressedHidden;
            bool noneShown = hiddenNow > 0 && (int)snapshot.rows.size() + hiddenNow == games;
            for (auto& row : snapshot.rows)
                noneShown = noneShown && !row.compressed;
            report("games already compressed are hidden to begin with", noneShown, format("%d shown, %d hidden of %d", (int)snapshot.rows.size(), hiddenNow, games));
            person.shot("real_all_compressed_hidden");
            // Going down the list with the stick, it's easily pushed a little to one side, which mustn't change the view.
            InputState diagonal;
            diagonal.held = ButtonDown;
            person.frame(diagonal);
            diagonal.held = ButtonDown | ButtonRight;
            person.frame(diagonal);
            person.settle(20);
            person.press(ButtonDown | ButtonLeft);
            report("pushing right or left on the way down doesn't change the view", app.snapshot().views[0] == "To do", app.snapshot().views[0]);
            person.press(ButtonRight);
            App::Snapshot done = app.snapshot();
            bool allDone = (int)done.rows.size() == hiddenNow;
            for (auto& row : done.rows)
                allDone = allDone && row.compressed;
            report("right shows them, under Done", done.views[0] == "Done" && allDone, format("%s, %d shown of %d", done.views[0].c_str(), (int)done.rows.size(), hiddenNow));
            // Done says what's become of each, rather than that compressing it would save nothing more, which is why it's there.
            std::string doneSays;
            bool nothingSaid = false, freedShown = false;
            for (auto& row : done.rows) {
                doneSays += row.name + ": " + row.second + " | " + row.saved + "; ";
                nothingSaid = nothingSaid || row.saved.find("Nothing") != std::string::npos || row.second.find("Nothing") != std::string::npos;
                freedShown = freedShown || (!row.saved.empty() && isdigit((unsigned char)row.saved[0]));
            }
            report("Done says what each freed, and never that it saves nothing more", !nothingSaid && freedShown, doneSays);
            person.shot("real_done_view");
            person.press(ButtonRight);
            report("and then everything", app.snapshot().views[0] == "Everything" && (int)app.snapshot().rows.size() == games);
            person.press(ButtonRight);
            report("and nothing further right", app.snapshot().views[0] == "Everything");
            person.press(ButtonLeft);
            person.press(ButtonLeft);
            report("and left goes back to what's to do", app.snapshot().views[0] == "To do" && app.snapshot().rows.size() == snapshot.rows.size());
            // Left on Everything, the list opens on Everything next time.
            person.press(ButtonRight);
            person.press(ButtonRight);
            report("Everything shows them all", app.snapshot().views[0] == "Everything" && (int)app.snapshot().rows.size() == games);
            person.shot("real_compressed_shown");
        }
        report("and the list opens on the view it was left on", readText(root + "/data/settings.txt").find("viewinstalled=2") != std::string::npos,
               readText(root + "/data/settings.txt"));

        // What's just been compressed goes the same way.
        auto console = freshConsole();
        writeText(root + "/data/settings.txt", "welcomed=1\nproblemsasked=1\nmeasureall=0\ndeleteasked=1\ninstallgamefiles=0\nminimumsaving=0\nfrompcnetwork=0\nfrompccable=0\n");
        sys::removeFile(root + "/config/settings.txt");
        games = (int)console->listApplications().size();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        int before = (int)app.snapshot().rows.size();
        person.press(ButtonX);
        person.press(ButtonA);
        person.waitUntilIdle();
        App::Snapshot snapshot = app.snapshot();
        report("what gets compressed goes off the list", before > 0 && snapshot.compressedHidden > 0 && (int)snapshot.rows.size() + snapshot.compressedHidden == before, format("%d before, %d shown, %d hidden, %s, %s", before, (int)snapshot.rows.size(), snapshot.compressedHidden, snapshot.results.empty() ? "" : snapshot.results[0].c_str(), readText(root + "/data/settings.txt").substr(0, 60).c_str()));
        report("and it's still in the results", !snapshot.results.empty());
        healthy(*console, "after hiding what was compressed");

        writeText(root + "/data/settings.txt", kept);
        sys::makeDirectories(root + "/config");
        writeText(root + "/config/settings.txt", keptElsewhere);
    }

    // A game too large for the memory there is gets said at the start, rather than hours later.
    {
        _putenv_s("HBP_TEST_WORK_MEMORY", "1000");
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        person.press(ButtonX);
        person.press(ButtonA);
        person.waitUntilIdle();
        std::string said;
        for (auto& line : app.snapshot().results)
            said += line + "\n";
        report("not enough memory for a game gets said", said.find("enough memory") != std::string::npos, said);
        healthy(*console, "after not having the memory");
        _putenv_s("HBP_TEST_WORK_MEMORY", "");
    }

    // Opened as an applet, it says so once, and what to do to have all the memory.
    {
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        platform.limited = true;
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 30);
        App::Snapshot said = app.snapshot();
        report("opened as an applet, it says so, and how to have all the memory", said.dialog == "Opened from the Album" && said.dialogText.find("hold R") != std::string::npos, said.dialog + ": " + said.dialogText);
        person.shot("real_applet");
        person.press(ButtonA);
        person.waitUntilIdle();
        person.settle();
        report("and only the once", app.snapshot().dialog.empty(), app.snapshot().dialog);
    }

    // A compressed game shouldn't have the console ask for it to be updated before it starts.
    {
        Bytes raw(0x20 + 0x10 + 0x20);
        raw[0x0C] = Cnmt::MetaApplication;
        wr16(&raw[0x0E], 0x10);
        wr32(&raw[0x2C], 0x50000);
        raw[0x14] = Cnmt::AttributeCompacted | 1;
        ContentMeta meta = ContentMeta::parse(raw, "a made up one");
        Bytes rebuilt = meta.rebuild(0, 0, {});
        report("a rebuilt game doesn't ask for a newer version of itself", rd32(&rebuilt[0x2C]) == 0, format("%u", rd32(&rebuilt[0x2C])));
        report("or say it needs its update to be whole", rebuilt[0x14] == 1, format("%02x", rebuilt[0x14]));

        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        person.press(ButtonX);
        person.press(ButtonA);
        person.waitUntilIdle();
        report("and the console gets told what version counts now", !console->launchVersions.empty());
        person.press(ButtonA);
    }

    // Setting it up afresh by removing its folder brings up the welcome screen, whatever's kept in the other place.
    {
        auto console = freshConsole();
        sys::removeFile(root + "/data/settings.txt");
        sys::removeFile(root + "/data/settings.txt.new");
        sys::makeDirectories(root + "/config");
        writeText(root + "/config/settings.txt", "welcomed=1\nproblemsasked=1\nmeasureall=0\ndeleteasked=1\ninstallgamefiles=0\nend\n");
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        report("a fresh folder brings up the welcome screen", app.showingWelcome());
        app.shutdown();
        startOver(root);
    }

    // Uninstalling and deleting from the + menu.
    {
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        size_t before = console->listApplications().size();
        chooseInMenu(person, app, "Uninstall this game");
        report("uninstalling asks first", app.snapshot().dialog.rfind("Uninstall ", 0) == 0, app.snapshot().dialog);
        person.shot("real_uninstall_confirm");
        person.press(ButtonA);
        report("and pressing A straight away keeps it", console->listApplications().size() == before);
        person.waitUntilIdle();
        chooseInMenu(person, app, "Uninstall this game");
        person.press(ButtonRight);
        person.press(ButtonA);
        person.waitUntilIdle();
        report("uninstalling takes it off the console", console->listApplications().size() + 1 == before, format("%d of %d", (int)console->listApplications().size(), (int)before));
        report("and says so", !app.snapshot().results.empty() && app.snapshot().results[0].find("Uninstalled") != std::string::npos, app.snapshot().results.empty() ? "" : app.snapshot().results[0]);
        person.press(ButtonA);
        report("and off the list", app.snapshot().rows.size() + 1 == before, format("%d", (int)app.snapshot().rows.size()));
        healthy(*console, "after uninstalling");

        auto nspsIn = [&](const std::string& folder) {
            int count = 0;
            for (auto& entry : sys::list(folder))
                count += endsWithNoCase(entry.name, ".nsp") ? 1 : 0;
            return count;
        };
        sys::makeDirectories(root + "/sd");
        writeText(root + "/sd/to delete.nsp", readText(nsps[0]));
        int files = nspsIn(root + "/sd");
        goToTab(person, app, 1);
        person.waitUntilIdle();
        chooseInMenu(person, app, "Delete this file");
        report("deleting a file asks first", app.snapshot().dialog.rfind("Delete ", 0) == 0, app.snapshot().dialog);
        person.press(ButtonRight);
        person.press(ButtonA);
        person.waitUntilIdle();
        report("and deletes it", nspsIn(root + "/sd") == files - 1, format("%d of %d", nspsIn(root + "/sd"), files));
        person.press(ButtonA);
        person.waitUntilIdle();
    }

    // Extracting what's already there is refused, rather than going over it.
    {
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        if (app.snapshot().tickHint == "Tick all")
            person.press(ButtonY);
        chooseInMenu(person, app, "Extract the ticked");
        person.waitUntilIdle();
        report("extracting works the first time", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle);
        person.press(ButtonA);
        person.waitUntilIdle();
        chooseInMenu(person, app, "Extract the ticked");
        person.waitUntilIdle();
        std::string said;
        for (auto& line : app.snapshot().results)
            said += line + "; ";
        report("and the second time says it's been done", said.find("It's already in") != std::string::npos, said);
        person.press(ButtonA);
        person.waitUntilIdle();
        sys::removeTree(root + "/output/extracted");
    }

    // Refreshing one list while another comes up leaves that one to finish afterwards.
    {
        sys::makeDirectories(root + "/sd/many");
        for (int i = 0; i < 1500; i++)
            writeText(root + "/sd/many/" + format("nothing %d.nsp", i), "not a game");
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        person.press(ButtonR);
        person.waitFor([&] { return app.snapshot().scanning; }, 5);
        person.press(ButtonL);
        chooseInMenu(person, app, "Refresh the list");
        person.waitUntilIdle();
        std::string log = readText(root + "/data/log.txt");
        report("the list that was left comes up by itself", log.find("of them opened") != std::string::npos && log.find("1500 game files") != std::string::npos, log.substr(log.size() > 400 ? log.size() - 400 : 0));
        sys::removeTree(root + "/sd/many");
    }

    // Everything ticked in one go, which it spells out first, as much of a list comes up ticked by itself.
    {
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        size_t before = console->listApplications().size();
        if (app.snapshot().tickHint == "Tick all")
            person.press(ButtonY);
        chooseInMenu(person, app, "Uninstall the ticked games");
        report("uninstalling several says how many", app.snapshot().dialog == format("Uninstall %d games?", (int)before), app.snapshot().dialog);
        person.shot("real_uninstall_many");
        person.press(ButtonA);
        report("and pressing A straight away keeps them", console->listApplications().size() == before);
        person.waitUntilIdle();
        chooseInMenu(person, app, "Uninstall the ticked games");
        person.press(ButtonRight);
        person.press(ButtonA);
        person.waitUntilIdle();
        report("and it takes them all", console->listApplications().empty(), format("%d left", (int)console->listApplications().size()));
        report("with a line for each", app.snapshot().results.size() == before, format("%d", (int)app.snapshot().results.size()));
        person.press(ButtonA);
        report("and they're off the list", app.snapshot().rows.empty());
        healthy(*console, "after uninstalling them all");
    }

    // Settings and the menu stay short to begin with.
    {
        auto console = freshConsole();
        writeText(root + "/data/settings.txt", "welcomed=1\nproblemsasked=1\nmeasureall=0\ndeleteasked=1\ninstallgamefiles=0\nfrompcnetwork=0\nfrompccable=0\n");
        sys::removeFile(root + "/config/settings.txt");
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        openSettings(person, app);
        person.shot("real_options_short");
        size_t few = app.snapshot().options.size();
        report("there's a log to begin with", sys::exists(root + "/data/log.txt"));
        // Analysis is among them, as what a first look isn't sure of may save more, and that's where a thorough one is chosen.
        report("the settings are short to begin with", few <= 9 && optionValue(app, "Compression level") == "(no such option)", format("%d", (int)few));
        goToOption(person, app, "More settings");
        person.press(ButtonA);
        person.settle();
        report("and the rest are there when asked for", app.snapshot().options.size() > few && optionValue(app, "Compression level") != "(no such option)", format("%d", (int)app.snapshot().options.size()));
        report("which is kept", readText(root + "/data/settings.txt").find("moresettings=1") != std::string::npos);
        goToTab(person, app, 0);
        person.press(ButtonPlus);
        report("the + menu is short too", app.snapshot().menu.size() <= 7, format("%d", (int)app.snapshot().menu.size()));
        person.shot("real_menu_short");
        person.press(ButtonB);
        startOver(root);
    }

    // A folder only goes if it's a game, and then all of it does.
    {
        auto console = freshConsole();
        std::string extractedTo = root + "/output/extracted";
        std::string folder;
        {
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(nsps[0]));
            set.group();
            folder = extractTitle(set.jobs.at(0), set, extractedTo).path;
        }
        writeText(root + "/output/made from it [LZ4].nsp", readText(nsps[0]));
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        goToTab(person, app, 1);
        person.waitUntilIdle();
        chooseInMenu(person, app, "Delete this folder");
        person.press(ButtonRight);
        person.press(ButtonA);
        person.waitUntilIdle();
        report("deleting a folder takes all of it", !sys::exists(folder), folder);
        person.press(ButtonA);
        person.waitUntilIdle();
        report("and what was made from it doesn't take its place on the list", app.snapshot().rows.empty() && sys::exists(root + "/output/made from it [LZ4].nsp"), format("%d", (int)app.snapshot().rows.size()));
    }

    // Going to another list doesn't hold up what's being worked out for this one, even while that list comes up.
    {
        sys::removeFile(root + "/data/analysis.txt");
        sys::removeFile(root + "/config/analysis.txt");
        forgetIndex(root);
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        report("analyzing starts", person.waitFor([&] { return app.snapshot().analyzing; }, 60));
        goToTab(person, app, 1);
        person.waitUntilIdle();
        std::string log = readText(root + "/data/log.txt");
        report("and isn't stopped by the next list coming up", log.find("Stopped analyzing") == std::string::npos, log.substr(log.size() > 500 ? log.size() - 500 : 0));
        goToTab(person, app, 0);
        App::Snapshot back = app.snapshot();
        bool allKnown = !back.rows.empty();
        for (auto& row : back.rows)
            allKnown = allKnown && row.hasEstimate;
        report("so it's all done when coming back", allKnown && !back.analyzing);
        healthy(*console, "after analyzing beside another list");
    }

    // Pressing X while it's still analyzing means getting on with it.
    {
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitFor([&] { return app.snapshot().analyzing || !app.busy(); }, 60);
        InputState down;
        down.held = ButtonX;
        person.frame(down);
        person.frame();
        bool asked = person.waitFor([&] { return app.snapshot().dialog.find("Compress") == 0; }, 60);
        report("compressing during the analysis stops it, and asks", asked, app.snapshot().dialog);
        person.shot("real_x_while_analyzing");
        person.press(ButtonB);
        person.settle();
        // Saying no means X wasn't wanted after all, so the analysis goes on, or has got to the end by now.
        bool goesOn = person.waitFor([&] {
            App::Snapshot now = app.snapshot();
            bool allKnown = !now.rows.empty();
            for (auto& row : now.rows)
                allKnown = allKnown && row.hasEstimate;
            return now.analyzing || allKnown;
        }, 30);
        report("saying no lets the analysis go on", goesOn);
    }

    // The firmware refuses: everything stays.
    {
        auto console = freshConsole();
        console->setFirmwareRefuses(true);
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        person.press(ButtonX);
        person.press(ButtonA);
        person.waitUntilIdle();
        report("a firmware that refuses stops everything at once", app.snapshot().resultsTitle == "Nothing was changed" && app.snapshot().results.empty(), app.snapshot().resultsTitle);
        report("and says what it takes", app.snapshot().resultsDetail.find("sigpatches") != std::string::npos, app.snapshot().resultsDetail);
        person.shot("real_refused");
        healthy(*console, "after the firmware refused");
        i64 total = 0;
        for (u64 id : console->listApplications())
            total += console->describe(id).totalBytes();
        report("and the games are the size they were", total == totalBefore);
    }

    // No room.
    {
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        console->setFree(Storage::Sd, 1 << 20);
        person.press(ButtonX);
        report("no room gets said before anything starts", app.snapshot().dialog == "There isn't enough room", app.snapshot().dialog);
        report("and nothing has started", app.snapshot().screen == "Main");
        person.shot("real_noroom");
        person.press(ButtonA);
        // What one game frees makes room for the next.
        console->setFree(Storage::Sd, 120LL << 20);
        person.press(ButtonX);
        App::Snapshot asked = app.snapshot();
        report("what one frees counts towards the next", asked.dialog.find("Compress these") == 0 && asked.dialogText.find("left out") == std::string::npos, asked.dialog + " " + asked.dialogText);
        person.press(ButtonB);

        // Saving NSPs frees nothing, so there's only room for some.
        goToOption(person, app, "Installed games");
        person.press(ButtonRight);
        goToTab(person, app, 0);
        person.settle();
        console->setFree(Storage::Sd, 100LL << 20);
        person.press(ButtonX);
        asked = app.snapshot();
        report("what doesn't fit gets said", asked.dialog.find("Compress this") == 0 && asked.dialogText.find("left out") != std::string::npos, asked.dialog + " " + asked.dialogText);
        printf("  with little room: %s\n", asked.dialogText.c_str());
        person.shot("real_room_for_some");
        person.press(ButtonB);
        console->setFree(Storage::Sd, -1);
        healthy(*console, "after there was no room");
        startOver(root);
    }

    // An NSP saved of an installed game leaves the game as it was, on the list, and the NSP is on Game files ready to install, as the
    // installed game isn't compressed. Done games hidden, as they are to begin with.
    {
        auto console = freshConsole();
        sys::renameFile(root + "/sd/games", root + "/games kept aside");
        sys::makeDirectories(root + "/sd/games");
        writeText(root + "/data/settings.txt", "welcomed=1\nproblemsasked=1\nmeasureall=0\ndeleteasked=1\nreplace=0\ninstallgamefiles=1\nminimumsaving=0\ncheckwritten=1\nhidecompressed=1\nmoresettings=1\nwritelog=1\nfrompcnetwork=0\nfrompccable=0\nnspfolder=" + root + "/sd/games\n");
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        size_t before = app.snapshot().rows.size();
        if (app.snapshot().tickHint == "Tick all")
            person.press(ButtonY);
        person.press(ButtonX);
        person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 30);
        person.press(ButtonA);
        person.waitUntilIdle();
        person.press(ButtonA);
        person.waitUntilIdle();
        report("installed games an NSP was saved of are still on the list", app.snapshot().rows.size() == before, format("%d of %d", (int)app.snapshot().rows.size(), (int)before));
        goToTab(person, app, 1);
        person.waitUntilIdle();
        int ready = 0;
        std::string seen;
        for (auto& row : app.snapshot().rows) {
            seen += row.name + ": " + row.note + " / " + row.detail + "; ";
            ready += row.ticked ? 1 : 0;
        }
        report("and the NSPs are on Game files, ticked to install", ready > 0, seen);
        report("where X says Install, which is what it does", app.snapshot().actionWord == "Install", app.snapshot().actionWord);
        person.shot("real_saved_then_listed");
        sys::removeTree(root + "/sd/games");
        sys::renameFile(root + "/games kept aside", root + "/sd/games");
        startOver(root);
    }

    // Stopping part way through.
    {
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        person.press(ButtonX);
        person.press(ButtonA);
        person.waitFor([&] { return app.snapshot().label.find("Compressing") == 0 || !app.busy(); }, 120);
        InputState down;
        down.held = ButtonB;
        person.frame(down);
        person.frame();
        person.shot("real_stop_dialog");
        report("stopping asks first", app.snapshot().dialog == "Stop?", app.snapshot().dialog);
        // The choices run left to right, and the last is to stop now.
        person.press(ButtonRight);
        person.press(ButtonRight);
        person.press(ButtonA);
        person.waitUntilIdle();
        report("it stops", app.snapshot().screen == "Results", app.snapshot().screen);
        person.shot("real_stopped");
        healthy(*console, "after stopping");
    }

    // The power goes while it's working, and the next start puts it right.
    {
        auto console = freshConsole();
        i64 before = 0;
        for (u64 id : console->listApplications())
            before += console->describe(id).totalBytes();
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            person.press(ButtonX);
            person.press(ButtonA);
            person.waitFor([&] { return app.snapshot().label.find("Checking every byte") == 0 || !app.busy(); }, 600);
            report("a game gets to the point of being read back", app.busy());
            console->losePowerAt(1);
            person.waitUntilIdle();
        }
        console->losePowerAt(0);
        auto restarted = std::make_unique<SimConsole>(consoleFolder, keyset);
        appearance(*restarted);
        HostPlatform platform(root, keys, restarted.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.settle();
        person.waitUntilIdle();
        report("the next start says what it did", app.snapshot().dialog == "Last time", app.snapshot().dialog);
        person.shot("real_after_power_cut");
        person.press(ButtonA);
        healthy(*restarted, "after a power cut");
    }
    refusedAtStart();
    goneFromConsole();
    savedUnderAFreeName();
    dlcInGameFile();
    solidCopyIsSaid();
    logRefused();
    logNamed();
    logToggled();
    plainReasons();
    walkLevels();
    answersKeptAsTheyAre();
    analyzedAgainFromDone();
    orderKeptWhenAnalyzedAgain();
    measuringWaitsForFirstLooks();
    analyzedAgainJoinsTheReading();
    turnedOffWhileLooking();

    // The rule, and ticking by hand.
    {
        auto console = freshConsole();
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
        }

        // These games save too much for the rule to apply, so the answers are made smaller than they were. They're taken as measured: a
        // first look under the minimum doesn't leave a game out, it leaves it not sure yet, unticked until it's measured.
        std::string answers = readText(root + "/data/analysis.txt"), changed;
        size_t position = 0;
        while (position < answers.size()) {
            size_t end = answers.find('\n', position);
            std::string line = answers.substr(position, end - position);
            position = end + 1;
            long long input, output, margin, romfs, stored;
            size_t tab = line.find('\t');
            if (tab != std::string::npos && sscanf(line.c_str() + tab + 1, "%lld %lld %lld %lld %lld", &input, &output, &margin, &romfs, &stored) == 5)
                line = line.substr(0, tab + 1) + format("%lld %lld %lld %lld %lld 0 1", input, input * 8 / 10, margin, romfs, stored);
            changed += line + "\n";
        }
        writeText(root + "/data/analysis.txt", changed);

        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        goToOption(person, app, "Minimum saving");
        for (int i = 0; i < 8; i++)
            person.press(ButtonRight);
        person.shot("real_options_minimum");
        goToTab(person, app, 0);
        person.settle();
        App::Snapshot snapshot = app.snapshot();
        bool noneTicked = !snapshot.rows.empty();
        for (auto& row : snapshot.rows)
            noneTicked = noneTicked && !row.ticked;
        report("a minimum of 50% leaves out games that save less", noneTicked);
        report("the setting is kept", readText(root + "/data/settings.txt").find("minimumsaving=50") != std::string::npos);
        person.shot("real_minimum");
        person.press(ButtonA);
        snapshot = app.snapshot();
        report("a game can still be ticked by hand", !snapshot.rows.empty() && snapshot.rows[0].ticked);

        // Hiding one, and bringing it back.
        size_t count = snapshot.rows.size();
        chooseInMenu(person, app, "Hide this game");
        person.settle();
        report("a game can be hidden", app.snapshot().rows.size() + 1 == count);
    }
    {
        auto console = std::make_unique<SimConsole>(consoleFolder, keyset);
        appearance(*console);
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.settle();
        person.waitUntilIdle();
        size_t hiddenCount = app.snapshot().rows.size();
        report("it stays hidden the next time", hiddenCount + 1 == console->listApplications().size());
        report("the minimum is still set", readText(root + "/data/settings.txt").find("minimumsaving=50") != std::string::npos);
        person.press(ButtonPlus);
        person.shot("real_menu_hidden");
        person.press(ButtonB);
        chooseInMenu(person, app, "Show the hidden");
        person.waitUntilIdle();
        report("hidden games can be brought back", app.snapshot().rows.size() == console->listApplications().size());
    }

    // Game files on the card: saved in the NSP folder, then installed.
    {
        auto console = freshConsole();
        const std::string& fileToKeep = nsps.size() > 2 ? nsps[2] : nsps[0];
        std::string kept = root + "/sd/games/" + fileToKeep.substr(fileToKeep.find_last_of("/\\") + 1);
        {
            sys::File in(fileToKeep, sys::File::Mode::Read);
            sys::File out(kept, sys::File::Mode::Create);
            Bytes buffer(1 << 20);
            i64 size = in.size();
            for (i64 done = 0; done < size;) {
                size_t n = (size_t)std::min<i64>((i64)buffer.size(), size - done);
                in.read(done, buffer.data(), n);
                out.write(done, buffer.data(), n);
                done += (i64)n;
            }
        }
        u8 before[32];
        Bytes sourceBytes = readWhole(kept);
        sha256(sourceBytes.data(), sourceBytes.size(), before);

        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        person.press(ButtonR);
        person.shot("real_files_scanning");
        person.waitUntilIdle();
        person.settle();
        person.shot("real_files");
        App::Snapshot snapshot = app.snapshot();
        report("the file is listed", snapshot.rows.size() == 1, format("%d", (int)snapshot.rows.size()));

        // A battery that's run low stops it before the next game, rather than letting the console run flat in the middle of writing.
        auto namesIn = [](const std::string& folder) {
            std::set<std::string> names;
            for (auto& entry : sys::list(folder))
                names.insert(entry.name);
            return names;
        };
        std::set<std::string> outputBefore = namesIn(root + "/output");
        platform.battery = 12;
        person.press(ButtonX);
        person.press(ButtonA);
        person.waitUntilIdle();
        report("a low battery stops it before the next game", app.snapshot().resultsTitle == "Stopped" && app.snapshot().resultsDetail.find("battery") != std::string::npos, app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        report("and nothing was written", namesIn(root + "/output") == outputBefore);
        person.press(ButtonA);
        platform.battery = 80;

        person.press(ButtonX);
        person.shot("real_files_confirm");
        person.press(ButtonA);
        person.waitUntilIdle();
        person.shot("real_files_results");
        report("saving an NSP finishes", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle);
        person.press(ButtonA);

        bool found = false;
        for (auto& entry : sys::list(root + "/output"))
            found = found || entry.name.find("[LZ4]") != std::string::npos;
        report("the NSP is in the NSP folder", found);
        Bytes after = readWhole(kept);
        u8 afterHash[32];
        sha256(after.data(), after.size(), afterHash);
        report("the file itself isn't changed", memcmp(before, afterHash, 32) == 0);

        // A look at it again only takes going through the folders.
        {
            std::string first = readText(root + "/data/log.txt");
            report("what's new got opened", first.find(", 1 of them opened") != std::string::npos, first.substr(first.size() > 400 ? first.size() - 400 : 0));
            report("what's known about it is kept", sys::exists(root + "/data/files.cache"));
        }

        // It's on the list of what's in the folders, as that's where it went.
        goToTab(person, app, 1);
        person.waitUntilIdle();
        person.settle();
        report("what's been saved doesn't make another entry for the same game", app.snapshot().rows.size() == 1, format("%d", (int)app.snapshot().rows.size()));
        person.shot("real_folders_with_nsp");

        // Installing it now, over the game that's installed with its update, which is newer than the file.
        goToOption(person, app, "Game files and folders");
        person.press(ButtonRight);
        goToTab(person, app, 1);
        person.waitUntilIdle();
        person.settle();
        report("the choice to install is kept", readText(root + "/data/settings.txt").find("installgamefiles=1") != std::string::npos);
        person.press(ButtonY);  // Tick everything: the file is done, but it hasn't been installed.
        person.press(ButtonX);
        person.shot("real_install_confirm");
        person.press(ButtonA);
        person.waitUntilIdle();
        person.shot("real_install_refused");
        std::string joined;
        for (auto& line : app.snapshot().results)
            joined += line + "\n";
        report("a file older than what's installed is refused", joined.find("newer version") != std::string::npos, joined);
        person.press(ButtonA);
        healthy(*console, "after refusing to install");
    }

    // Installing on a console that hasn't got the game.
    {
        sys::removeTree(root + "/console2");
        sys::removeTree(root + "/output");
        sys::removeFile(root + "/data/list.cache");
        SimConsole console(root + "/console2", keyset);
        HostPlatform platform(root, keys, &console);
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        person.shot("real_nothing_installed");
        person.press(ButtonR);
        person.waitUntilIdle();
        person.settle();
        person.press(ButtonX);
        person.press(ButtonA);
        person.waitUntilIdle();
        person.shot("real_install_results");
        report("installing finishes", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        person.press(ButtonA);
        person.press(ButtonL);
        person.settle();
        person.shot("real_installed_now");
        report("and the game is listed as installed", app.snapshot().rows.size() + app.snapshot().compressedHidden == 1, format("%d", (int)app.snapshot().rows.size()));
        std::string problem = console.checkConsistent(keyset);
        report("after installing: it's whole", problem.empty(), problem);
        auto stray = console.strayFiles();
        report("after installing: nothing left lying around", stray.empty(), stray.empty() ? "" : stray[0]);
        startOver(root);
    }

    // Deleting a file once it's installed, as long as nothing else in it still needs it.
    {
        sys::removeTree(root + "/console3");
        sys::removeFile(root + "/data/list.cache");
        std::string file;
        for (auto& entry : sys::list(root + "/sd/games"))
            file = root + "/sd/games/" + entry.name;
        SimConsole console(root + "/console3", keyset);
        HostPlatform platform(root, keys, &console);
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        goToOption(person, app, "Game files and folders");
        person.press(ButtonRight);
        goToOption(person, app, "Once a file is installed");
        person.press(ButtonRight);
        person.shot("real_options_delete");
        goToTab(person, app, 0);
        report("the choice to delete is kept", readText(root + "/data/settings.txt").find("deleteinstalled=1") != std::string::npos);
        person.press(ButtonR);
        person.waitUntilIdle();
        person.settle();
        person.press(ButtonX);
        report("it says the file will go", app.snapshot().dialogText.find("deleted") != std::string::npos, app.snapshot().dialogText);
        person.shot("real_delete_confirm");
        Bytes source = readWhole(file);
        person.press(ButtonA);
        person.waitUntilIdle();
        person.shot("real_delete_results");
        report("installing and deleting finishes", app.snapshot().resultsTitle == "Finished" && app.snapshot().resultsDetail.find("deleted") != std::string::npos, app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        report("the file is gone", !sys::exists(file));
        person.press(ButtonA);
        person.waitUntilIdle();
        report("and off the list", app.snapshot().rows.empty(), format("%d", (int)app.snapshot().rows.size()));
        person.press(ButtonL);
        person.waitUntilIdle();
        report("while the game is installed", app.snapshot().rows.size() == 1, format("%d", (int)app.snapshot().rows.size()));
        std::string problem = console.checkConsistent(keyset);
        report("and whole", problem.empty(), problem);

        // Back as it was for what comes next.
        sys::File out(file, sys::File::Mode::Create);
        out.write(0, source.data(), source.size());
        startOver(root);
    }

    // Extracting.
    {
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        chooseInMenu(person, app, "Extract the ticked");
        person.shot("real_extracting");
        person.waitUntilIdle();
        person.shot("real_extracted");
        report("extracting finishes", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle);
        report("the files are there", sys::isDirectory(root + "/output/extracted"));
        healthy(*console, "after extracting");
    }

    // The game this app was opened through can't be replaced while it's in use, even if it was ticked before that was known.
    {
        auto console = freshConsole();
        u64 host = console->listApplications().at(0);
        i64 before = console->describe(host).totalBytes();
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            if (app.snapshot().tickHint == "Tick all")
                person.press(ButtonY);
            // As with the list remembered from last time, until it's been brought up to date.
            console->runsThrough = host;
            person.press(ButtonX);
            person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 30);
            person.press(ButtonA);
            person.waitUntilIdle();
            std::string said;
            for (auto& line : app.snapshot().results)
                said += line + "; ";
            report("the game this app runs through isn't replaced, even when it was ticked",
                console->describe(host).totalBytes() == before && said.find("open it from another one") != std::string::npos, app.snapshot().resultsTitle + ": " + said);
            healthy(*console, "after turning down the game it runs through");
        }
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            bool shown = false;
            for (auto& row : app.snapshot().rows)
                shown = shown || (row.note.find("open it from another one") != std::string::npos && !row.ticked);
            report("and the list says why, with it unticked", shown);
        }
        console->runsThrough = 0;
    }

    // Help, and a report of what happened that never has a key in it, even when one gets somewhere it's read from.
    {
        auto console = freshConsole();
        std::vector<std::string> secrets;
        std::string planted;
        std::string keyText = readText(keys);
        for (size_t start = 0; start < keyText.size();) {
            size_t end = keyText.find('\n', start);
            if (end == std::string::npos)
                end = keyText.size();
            std::string line = keyText.substr(start, end - start);
            start = end + 1;
            size_t equals = line.find('=');
            if (equals == std::string::npos)
                continue;
            std::string value;
            for (char c : line.substr(equals + 1)) {
                if (isxdigit((unsigned char)c))
                    value += (char)tolower((unsigned char)c);
            }
            if (value.size() >= 16)
                secrets.push_back(value);
            if (line.rfind("header_key", 0) == 0)
                planted = line;
        }
        writeText(root + "/data/opening.txt", "Opening\n" + planted + "\n");
        // Atmosphère's reports of what crashed: a recent one with half a key in a register and the whole of it in what it
        // copied out of memory, one of a fatal error, and one from long ago.
        std::string headerKey;
        for (char c : planted.substr(planted.find('=') + 1)) {
            if (isxdigit((unsigned char)c))
                headerKey += (char)toupper((unsigned char)c);
        }
        std::string backwards;
        for (size_t i = 16; i >= 2; i -= 2)
            backwards += headerKey.substr(i - 2, 2);
        std::string dump = "    0000000000000000:";
        for (size_t i = 0; i + 2 <= headerKey.size() && i < 32; i += 2)
            dump += " " + headerKey.substr(i, 2);
        long long now = (long long)time(nullptr);
        sys::makeDirectories(root + "/atmosphere/crash_reports");
        sys::makeDirectories(root + "/atmosphere/fatal_reports");
        writeText(root + format("/atmosphere/crash_reports/%011lld_0100000000001000.log", now - 60),
            "Atmosph\xC3\xA8re Crash Report (v1.12.1)\nCrash Report:\n    Title ID:          0100000000001000\nCrashed Thread Info:\n    Registers:\n        X0:  0x" +
            backwards + "\n        X1:  0x0000000000000001\n    Stack Dump:\n" + dump + "\n");
        writeText(root + format("/atmosphere/fatal_reports/%011lld_010041544d530000.log", now - 120), "Atmosph\xC3\xA8re Fatal Report (v1.12.1):\nResult: 0x1ffe (2162-0002)\n");
        writeText(root + format("/atmosphere/crash_reports/%011lld_0100000000002000.log", now - 90LL * 24 * 3600), "Long ago\n");
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();

        chooseInMenu(person, app, "A game won't start");
        report("a help topic opens from the + menu", app.snapshot().dialog == "A game won't start", app.snapshot().dialog);
        person.shot("real_help_topic");
        person.press(ButtonA);

        // Cancelling the keyboard cancels the report, and B on the question after it neither sends nor saves it: B on the keyboard had it go.
        auto confirmReport = [&](const char* asked) {
            bool ok = app.snapshot().dialog == asked;
            report(std::string("it's asked first: ") + asked, ok, app.snapshot().dialog);
            if (ok)
                person.press(ButtonA);
        };
        sys::removeFile(root + "/data/report.txt");
        platform.keyboardCancelled = true;
        chooseInMenu(person, app, "Report a problem");
        person.settle();
        report("cancelling the keyboard cancels the report", app.snapshot().dialog.empty() && !sys::exists(root + "/data/report.txt"), app.snapshot().dialog);
        platform.keyboardCancelled = false;
        chooseInMenu(person, app, "Report a problem");
        report("a report is asked about before it goes anywhere", app.snapshot().dialog == "Save this report?", app.snapshot().dialog);
        person.shot("real_report_asked");
        person.press(ButtonB);
        person.settle();
        report("and B there doesn't save or send it", app.snapshot().dialog.empty() && !sys::exists(root + "/data/report.txt"), app.snapshot().dialog);
        chooseInMenu(person, app, "Report a problem");
        confirmReport("Save this report?");
        report("a report gets saved", app.snapshot().dialog == "Report saved", app.snapshot().dialog + ": " + app.snapshot().dialogText);
        person.shot("real_report_saved");
        person.press(ButtonA);
        std::string saved = sys::exists(root + "/data/report.txt") ? readText(root + "/data/report.txt") : "";
        std::string lower = saved;
        for (auto& c : lower)
            c = (char)tolower((unsigned char)c);
        std::string leaked;
        for (auto& secret : secrets) {
            if (lower.find(secret) != std::string::npos)
                leaked = secret.substr(0, 4) + "...";
        }
        report("with the version, the lists and what was said", saved.find(std::string("hacBrewPackLZ ") + AppVersion) == 0 && saved.find("== Installed games") != std::string::npos && saved.find("== The last") != std::string::npos);
        report("and no key in it, even one put where it's read from", !planted.empty() && leaked.empty() && saved.find("[key left out]") != std::string::npos, leaked);
        std::string lowerKey = headerKey, lowerBackwards = backwards;
        for (auto* s : {&lowerKey, &lowerBackwards}) {
            for (auto& c : *s)
                c = (char)tolower((unsigned char)c);
        }
        report("with what Atmosph\xC3\xA8re says crashed lately, both kinds", saved.find("Crash Report:") != std::string::npos && saved.find("Result: 0x1ffe") != std::string::npos);
        report("but not long ago", saved.find("Long ago") == std::string::npos);
        report("and without what it copied out of memory, or a register holding part of a key",
            saved.find("lines copied out of memory left out") != std::string::npos && lower.find(lowerBackwards) == std::string::npos &&
                lower.find(lowerKey.substr(0, 2) + " " + lowerKey.substr(2, 2) + " " + lowerKey.substr(4, 2)) == std::string::npos &&
                saved.find("X0:  0x[left out]") != std::string::npos && saved.find("X1:  0x0000000000000001") != std::string::npos,
            format("dump left out %d, register %d %d, bytes %d, X1 %d", saved.find("lines copied out of memory left out") != std::string::npos,
                lower.find(lowerBackwards) == std::string::npos, saved.find("X0:  0x[left out]") != std::string::npos,
                lower.find(lowerKey.substr(0, 2) + " " + lowerKey.substr(2, 2) + " " + lowerKey.substr(4, 2)) == std::string::npos,
                saved.find("X1:  0x0000000000000001") != std::string::npos));
        // With the report service there, it goes straight to it, with what was typed on the keyboard, and no key in that either.
        _putenv_s("HBP_TEST_REPORT_SITE", "https://reports.test");
        platform.online = true;
        platform.typed = "It froze while compressing";
        auto outcome = [&](const char* title) {
            person.waitFor([&] { return !platform.posted.empty() || app.snapshot().dialog == title; }, 30);
            if (app.snapshot().dialog == "Sending the report")
                person.press(ButtonA);
            person.waitFor([&] { return app.snapshot().dialog == title; }, 30);
            return app.snapshot().dialog == title;
        };
        chooseInMenu(person, app, "Report a problem");
        confirmReport("Send this report?");
        bool sentOk = outcome("Report sent");
        std::string sent = platform.posted.empty() ? "" : platform.posted.back().second;
        report("with the report service there, a report goes to it", sentOk && !platform.posted.empty() && platform.posted.back().first == "https://reports.test/report" &&
            app.snapshot().dialogText.find("report 1") != std::string::npos, app.snapshot().dialog + ": " + app.snapshot().dialogText);
        person.shot("real_report_sent");
        report("with what was typed, and what it is", sent.find("\"said\":\"It froze while compressing\"") != std::string::npos &&
            sent.find("\"platform\":\"Switch\"") != std::string::npos && sent.find("\"kind\":\"manual\"") != std::string::npos, sent.substr(0, 200));
        std::string sentLower = sent;
        for (auto& c : sentLower)
            c = (char)tolower((unsigned char)c);
        std::string sentLeak;
        for (auto& secret : secrets) {
            if (sentLower.find(secret) != std::string::npos)
                sentLeak = secret.substr(0, 4) + "...";
        }
        report("and no key in what's sent either", sentLeak.empty() && sent.find("[key left out]") != std::string::npos, sentLeak);
        person.press(ButtonA);

        platform.postReply = "{\"status\":\"fixed\",\"fixedIn\":\"1.5.1\",\"issue\":42,\"id\":2}";
        platform.posted.clear();
        chooseInMenu(person, app, "Report a problem");
        confirmReport("Send this report?");
        report("a known problem says what fixes it", outcome("A known problem") && app.snapshot().dialogText.find("1.5.1") != std::string::npos,
            app.snapshot().dialog + ": " + app.snapshot().dialogText);
        person.press(ButtonA);

        platform.postCode = 429;
        platform.postReply = "{\"error\":\"too many reports for now, try again later\"}";
        platform.posted.clear();
        sys::removeFile(root + "/data/report.txt");
        chooseInMenu(person, app, "Report a problem");
        confirmReport("Send this report?");
        bool turnedAway = outcome("Report saved instead");
        std::string savedInstead = sys::exists(root + "/data/report.txt") ? readText(root + "/data/report.txt") : "";
        report("turned away, it says why and saves it, with what was typed first", turnedAway && app.snapshot().dialogText.find("too many reports") != std::string::npos &&
            savedInstead.rfind("What I was doing: It froze while compressing", 0) == 0, app.snapshot().dialog + ": " + app.snapshot().dialogText);
        person.press(ButtonA);

        platform.online = false;
        platform.postCode = 200;
        sys::removeFile(root + "/data/report.txt");
        chooseInMenu(person, app, "Report a problem");
        confirmReport("Save this report?");
        std::string offline = sys::exists(root + "/data/report.txt") ? readText(root + "/data/report.txt") : "";
        report("with no internet, it's saved as before", app.snapshot().dialog == "Report saved" && offline.rfind("What I was doing: It froze while compressing", 0) == 0,
            app.snapshot().dialog);
        person.press(ButtonA);
        _putenv_s("HBP_TEST_REPORT_SITE", "none");
        platform.typed.clear();
        sys::removeTree(root + "/atmosphere");
        sys::removeFile(root + "/data/opening.txt");
    }

    // Whoever had the welcome before sending problems to the developer was on it, as on 1.4.3, is asked the once, and either answer is kept.
    {
        auto console = freshConsole();
        std::string settingsPath = root + "/data/settings.txt";
        std::string settingsWere = sys::exists(settingsPath) ? readText(settingsPath) : "";
        for (bool send : {false, true}) {
            // As 1.4.3 left them: welcomed, and nothing said about sending problems. The app goes by either copy, so there's one.
            writeText(settingsPath, "welcomed=1\ninstallgamefiles=0\nfrompcnetwork=0\nfrompccable=0\n");
            sys::removeFile(root + "/config/settings.txt");
            {
                HostPlatform platform(root, keys, console.get());
                App app(platform);
                app.start();
                Person person(app, shots);
                person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 10);
                std::string asked = app.snapshot().dialog;
                if (!send)
                    person.shot("real_send_problems_question");
                report(send ? "asked again, it's the same question" : "someone who had the welcome before is asked about sending problems",
                    asked == "Send problem reports?", asked + ": " + app.snapshot().dialogText);
                if (!send)
                    person.press(ButtonRight);
                person.press(ButtonA);
                person.waitUntilIdle();
                std::string saved = readText(settingsPath);
                report(send ? "Send them turns it on, and that's kept" : "No thanks leaves it off, and that's kept",
                    saved.find("problemsasked=1") != std::string::npos && saved.find(send ? "sendproblems=1" : "sendproblems=0") != std::string::npos, saved);
            }
            {
                HostPlatform platform(root, keys, console.get());
                App app(platform);
                app.start();
                Person person(app, shots);
                person.waitUntilIdle();
                report(send ? "and then it isn't asked again either" : "and then it isn't asked again", app.snapshot().dialog.empty(), app.snapshot().dialog);
            }
        }
        writeText(settingsPath, settingsWere);
        sys::removeFile(root + "/config/settings.txt");
    }

    // Problems the app finds itself go to the developer by itself, once that's agreed to: a game that couldn't be done straight away, and the
    // app having stopped, or having stopped answering until it was closed, the next time it opens. Each once a version, and counted after
    // that. A compressed game that stopped with an error since the app was last open is asked about, as is one the console's play history has
    // closing within moments of starting. Turned off, what was waiting is dropped.
    if (!nsps.empty()) {
        auto console = freshConsole();
        std::string settingsPath = root + "/data/settings.txt";
        std::string settingsWere = sys::exists(settingsPath) ? readText(settingsPath) : "";
        writeText(settingsPath, "welcomed=1\nproblemsasked=1\nmeasureall=0\ndeleteasked=1\ninstallgamefiles=0\nminimumsaving=0\nhidecompressed=0\nfrompcnetwork=0\nfrompccable=0\nwritelog=1\nsendproblems=1\n");
        // The app goes by either copy of its settings, so the other one goes, as it may say otherwise.
        sys::removeFile(root + "/config/settings.txt");
        sys::removeTree(root + "/data/reports waiting");
        sys::removeFile(root + "/data/problems sent.txt");
        sys::removeTree(root + "/atmosphere");
        sys::makeDirectories(root + "/atmosphere/crash_reports");
        _putenv_s("HBP_TEST_REPORT_SITE", "https://reports.test");
        auto sentTo = [](HostPlatform& platform, const char* where) {
            std::vector<std::string> found;
            for (auto& post : platform.posted) {
                if (post.first == std::string("https://reports.test") + where)
                    found.push_back(post.second);
            }
            return found;
        };
        auto opened = [&](HostPlatform& platform, App& app, Person& person, size_t expected) {
            person.waitFor([&] { return platform.posted.size() >= expected; }, 15);
            person.waitUntilIdle();
            sys::sleepMs(300);
        };

        // Not answering, until it was closed: the note it left goes the next time it opens.
        writeText(root + "/data/not answering.txt", "Stopped answering while compressing\n\nThe last 2 lines of what the app said:\n[1] Compressing a game\n[2] Writing\n");
        {
            HostPlatform platform(root, keys, console.get());
            platform.online = true;
            App app(platform);
            app.start();
            Person person(app, shots);
            opened(platform, app, person, 1);
            auto reports = sentTo(platform, "/report");
            std::string sent = reports.empty() ? "" : reports[0];
            report("a freeze the app never came out of goes to the developer the next time it opens", reports.size() == 1 && sent.find("\"kind\":\"freeze\"") != std::string::npos &&
                sent.find("\"title\":\"Stopped answering while compressing\"") != std::string::npos && sent.find("Writing") != std::string::npos &&
                !sys::exists(root + "/data/not answering.txt"), sent.substr(0, 300));
        }

        // Stopping on something it didn't expect, which Atmosphère wrote up, goes the next time it opens, with no registers in what it says.
        long long now = (long long)time(nullptr);
        std::string crash = "Atmosph\xC3\xA8re Crash Report (v1.7):\nResult:                          0x4A8 (2168-0002)\n\nException Info:\n    Type:                        Data Abort\n"
            "Crashed Thread Info:\n    Registers:\n        X[00]:                   00000000DEADBEEF\n        LR:                      0000000012345678 (hacBrewPackLZ + 0x1645a0)\n"
            "        PC:                      0000000012345600 (hacBrewPackLZ + 0x164460)\nModule Info:\n    Module 00:\n        Name:                    hacBrewPackLZ\n";
        writeText(root + "/data/running.txt", std::to_string(now - 100));
        writeText(root + format("/atmosphere/crash_reports/%011lld_050e2a436f15f000.log", now - 50), crash);
        std::string crashFingerprint;
        {
            HostPlatform platform(root, keys, console.get());
            platform.online = true;
            App app(platform);
            app.start();
            Person person(app, shots);
            opened(platform, app, person, 1);
            auto reports = sentTo(platform, "/report");
            std::string sent = reports.empty() ? "" : reports[0];
            std::string summary = sent.substr(0, sent.find("\\n\\n"));
            report("the app having stopped goes the next time it opens, as what Atmosph\xC3\xA8re says of it",
                reports.size() == 1 && sent.find("\"kind\":\"crash\"") != std::string::npos &&
                    sent.find("\"title\":\"Stopped: Data Abort at hacBrewPackLZ + 0x164460 (2168-0002)\"") != std::string::npos &&
                    summary.find("PC: hacBrewPackLZ + 0x164460") != std::string::npos && summary.find("DEADBEEF") == std::string::npos,
                sent.substr(0, 400));
            size_t at = sent.find("\"fingerprint\":\"");
            crashFingerprint = at == std::string::npos ? "" : sent.substr(at + 15, sent.find('"', at + 15) - at - 15);
        }
        // The same again, in the same version, is only counted, which goes as a count.
        writeText(root + "/data/running.txt", std::to_string(now - 20));
        writeText(root + format("/atmosphere/crash_reports/%011lld_050e2a436f15f000.log", now - 10), crash);
        {
            HostPlatform platform(root, keys, console.get());
            platform.online = true;
            App app(platform);
            app.start();
            Person person(app, shots);
            opened(platform, app, person, 1);
            auto counted = sentTo(platform, "/seen");
            report("the same problem again is only counted", sentTo(platform, "/report").empty() && counted.size() == 1 &&
                counted[0].find("\"count\":1") != std::string::npos && !crashFingerprint.empty() && counted[0].find(crashFingerprint) != std::string::npos,
                counted.empty() ? std::string("nothing") : counted[0]);
        }
        sys::removeTree(root + "/atmosphere");
        sys::makeDirectories(root + "/atmosphere/crash_reports");

        // Not answering for a while leaves a note, which goes once it answers again.
        _putenv_s("HBP_TEST_FREEZE_SECONDS", "1");
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            sys::sleepMs(3500);
            bool noted = sys::exists(root + "/data/not answering.txt");
            std::string note = noted ? readText(root + "/data/not answering.txt") : "";
            person.waitUntilIdle();
            sys::sleepMs(800);
            person.waitUntilIdle();
            report("not answering for a while leaves a note of what it was doing, gone once it answers again",
                noted && note.rfind("Stopped answering while ", 0) == 0 && !sys::exists(root + "/data/not answering.txt"), note.substr(0, 120));
        }
        _putenv_s("HBP_TEST_FREEZE_SECONDS", "");
        report("closing as it should leaves nothing to tell next time", !sys::exists(root + "/data/running.txt") && !sys::exists(root + "/data/not answering.txt"));

        // A game a job can't do goes straight away.
        _putenv_s("HBP_TEST_PROBLEM", "job");
        {
            HostPlatform platform(root, keys, console.get());
            platform.online = true;
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            if (app.snapshot().tickHint == "Tick all")
                person.press(ButtonY);
            person.press(ButtonX);
            person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 30);
            person.press(ButtonA);
            person.waitUntilIdle();
            opened(platform, app, person, 1);
            auto reports = sentTo(platform, "/report");
            std::string sent = reports.empty() ? "" : reports[0];
            report("a game a job couldn't do goes to the developer straight away, without the names of files",
                reports.size() == 1 && sent.find("\"kind\":\"failed-job\"") != std::string::npos &&
                    sent.find("\"title\":\"Couldn't write '...' (2002-0002), for the tests") != std::string::npos,
                sent.substr(0, 300));
            person.press(ButtonA);
            person.waitUntilIdle();
        }
        _putenv_s("HBP_TEST_PROBLEM", "");

        // A file system the system says is damaged stops the job at the game it happened on, with what to do about it, and nothing more
        // is written: the other games aren't gone on to.
        _putenv_s("HBP_TEST_PROBLEM", "damaged");
        {
            HostPlatform platform(root, keys, console.get());
            // Its report goes out here, rather than waiting for the next app, which has a report of its own to check.
            platform.online = true;
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            if (app.snapshot().tickHint == "Tick all")
                person.press(ButtonY);
            int ticked = 0;
            for (auto& row : app.snapshot().rows)
                ticked += row.ticked ? 1 : 0;
            person.press(ButtonX);
            person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 30);
            person.press(ButtonA);
            person.waitUntilIdle();
            auto s = app.snapshot();
            person.shot("damaged_file_system");
            report("a damaged file system stops the job at the first game, saying what to do", ticked > 1 && s.resultsTitle == "Stopped: a damaged file system" &&
                s.results.size() == 1 && s.results[0].find("(2002-4690). The system says the file system it was writing to is damaged") != std::string::npos &&
                s.results[0].find("H2testw") != std::string::npos,
                format("%d ticked, %s | %s", ticked, s.resultsTitle.c_str(), s.results.empty() ? "" : s.results[0].c_str()));
            person.press(ButtonA);
            person.waitUntilIdle();
        }
        // Found by the check that this console reads what's made, nothing is changed, and it says what to do.
        _putenv_s("HBP_TEST_PROBLEM", "damaged check");
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            if (app.snapshot().tickHint == "Tick all")
                person.press(ButtonY);
            person.press(ButtonX);
            person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 30);
            person.press(ButtonA);
            person.waitUntilIdle();
            auto s = app.snapshot();
            person.shot("damaged_at_the_check");
            report("a damaged file system found by the console check changes nothing, saying what to do",
                s.resultsTitle == "Nothing was changed" && s.results.empty() && s.resultsAdvice.find("is damaged, so nothing more was written") != std::string::npos,
                s.resultsTitle + " | " + s.resultsDetail + " | " + s.resultsAdvice);
            person.press(ButtonA);
            person.waitUntilIdle();
        }
        _putenv_s("HBP_TEST_PROBLEM", "");

        // A compressed game that stopped with an error since the app was last open is asked about, and not starting goes to the developer.
        u64 application = console->listApplications().at(0);
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            if (app.snapshot().tickHint == "Tick all")
                person.press(ButtonY);
            person.press(ButtonX);
            person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 30);
            person.press(ButtonA);
            person.waitUntilIdle();
            person.press(ButtonA);
            person.waitUntilIdle();
        }
        sys::sleepMs(1100);
        writeText(root + format("/atmosphere/crash_reports/%011lld_%016llx.log", (long long)time(nullptr), (unsigned long long)application),
            "Atmosph\xC3\xA8re Crash Report (v1.7):\nResult:                          0x4A8 (2168-0002)\n");
        {
            HostPlatform platform(root, keys, console.get());
            platform.online = true;
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitFor([&] { return app.snapshot().dialog.rfind("Did ", 0) == 0; }, 60);
            std::string asked = app.snapshot().dialog;
            person.shot("real_did_it_start");
            report("a compressed game that stopped with an error since last time is asked about", asked.rfind("Did ", 0) == 0 && asked.back() == '?',
                asked + ": " + app.snapshot().dialogText);
            person.press(ButtonRight);
            person.press(ButtonA);
            person.waitFor([&] { return !sentTo(platform, "/report").empty(); }, 15);
            auto reports = sentTo(platform, "/report");
            std::string sent = reports.empty() ? "" : reports[0];
            report("not starting says what usually helps, and goes to the developer with the game's name, known by its title ID",
                app.snapshot().dialog == "What usually helps" && sent.find("\"kind\":\"game-didnt-start\"") != std::string::npos &&
                    sent.find(format("[%016llX] didn't start after being compressed", (unsigned long long)application)) != std::string::npos &&
                    sent.find(format("\"fingerprint\":\"game-didnt-start|%016llx\"", (unsigned long long)application)) != std::string::npos,
                app.snapshot().dialog + " / " + sent.substr(0, 300));
            person.shot("real_what_usually_helps");
            person.press(ButtonA);
            person.waitUntilIdle();
        }
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            report("and it's only asked the once", app.snapshot().dialog.empty(), app.snapshot().dialog);
        }

        // A compressed game the console's play history has closing a few seconds after it started is asked about too, though Atmosphère wrote
        // nothing down, as with the system's error screen or a game that closes by itself. One played for minutes isn't, and it's asked the
        // once. Each time but the last, the app was last open ten minutes ago, before all the history holds.
        sys::removeTree(root + "/atmosphere");
        sys::makeDirectories(root + "/atmosphere/crash_reports");
        long long tenMinutesAgo = (long long)time(nullptr) - 600;
        GamePlayed playedForMinutes{application, tenMinutesAgo + 100, tenMinutesAgo + 220};
        GamePlayed closedSoon{application, tenMinutesAgo + 300, tenMinutesAgo + 303};
        writeText(root + "/data/last opened.txt", std::to_string(tenMinutesAgo));
        {
            HostPlatform platform(root, keys, console.get());
            platform.played = {playedForMinutes};
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            person.settle(120);
            report("a compressed game the play history has playing for minutes isn't asked about", app.snapshot().dialog.empty(), app.snapshot().dialog);
        }
        writeText(root + "/data/last opened.txt", std::to_string(tenMinutesAgo));
        {
            HostPlatform platform(root, keys, console.get());
            platform.played = {playedForMinutes, closedSoon};
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitFor([&] { return app.snapshot().dialog.rfind("Did ", 0) == 0; }, 60);
            std::string asked = app.snapshot().dialog, said = app.snapshot().dialogText;
            person.shot("real_did_it_start_play_history");
            report("a compressed game the play history has closing 3 seconds after it started is asked about, with nothing from Atmosph\xC3\xA8re",
                asked.rfind("Did ", 0) == 0 && asked.back() == '?' && said.find("closing 3 seconds after it started") != std::string::npos &&
                    said.find("Atmosph") == std::string::npos && said.find("Another") == std::string::npos,
                asked + ": " + said);
            person.press(ButtonA);
            person.waitUntilIdle();
        }
        {
            HostPlatform platform(root, keys, console.get());
            platform.played = {playedForMinutes, closedSoon};
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            person.settle(120);
            report("and that's only asked the once too", app.snapshot().dialog.empty(), app.snapshot().dialog);
        }

        // With sending off, not starting offers to send it, which goes in the one press, without the keyboard: there's nothing more to say.
        writeText(settingsPath, "welcomed=1\nproblemsasked=1\nmeasureall=0\ndeleteasked=1\ninstallgamefiles=0\nfrompcnetwork=0\nfrompccable=0\nsendproblems=0\n");
        sys::removeFile(root + "/config/settings.txt");
        sys::sleepMs(1100);
        writeText(root + format("/atmosphere/crash_reports/%011lld_%016llx.log", (long long)time(nullptr), (unsigned long long)application),
            "Atmosph\xC3\xA8re Crash Report (v1.7):\nResult:                          0x4A8 (2168-0002)\n");
        {
            HostPlatform platform(root, keys, console.get());
            platform.online = true;
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitFor([&] { return app.snapshot().dialog.rfind("Did ", 0) == 0; }, 60);
            person.press(ButtonRight);
            person.press(ButtonA);
            report("with sending off, not starting offers to send it", app.snapshot().dialog == "What usually helps", app.snapshot().dialog);
            person.press(ButtonA);
            person.waitFor([&] { return !sentTo(platform, "/report").empty(); }, 15);
            auto reports = sentTo(platform, "/report");
            std::string sent = reports.empty() ? "" : reports[0];
            report("which goes in the one press, without the keyboard", platform.keyboardOpened == 0 && sent.find("\"kind\":\"game-didnt-start\"") != std::string::npos &&
                sent.find(format("\"fingerprint\":\"game-didnt-start|%016llx\"", (unsigned long long)application)) != std::string::npos &&
                sent.find("\"said\":\"\"") != std::string::npos, format("keyboard %d, ", platform.keyboardOpened) + sent.substr(0, 300));
            person.waitFor([&] { return app.snapshot().dialog == "Report sent"; }, 15);
            person.press(ButtonA);
            person.waitUntilIdle();
        }

        // Turned off, what was waiting is dropped, and nothing goes.
        sys::makeDirectories(root + "/data/reports waiting");
        writeText(root + "/data/reports waiting/crash-0123456789ab.txt", "kind=crash\nfingerprint=crash|0123456789ab\ntitle=Stopped\nversion=1\n\nwhat happened\n");
        writeText(settingsPath, "welcomed=1\nproblemsasked=1\nmeasureall=0\ndeleteasked=1\ninstallgamefiles=0\nfrompcnetwork=0\nfrompccable=0\nsendproblems=0\n");
        sys::removeFile(root + "/config/settings.txt");
        {
            HostPlatform platform(root, keys, console.get());
            platform.online = true;
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitFor([&] { return !sys::exists(root + "/data/reports waiting"); }, 10);
            person.waitUntilIdle();
            report("turned off, what was waiting is dropped, and nothing goes", !sys::exists(root + "/data/reports waiting") && platform.posted.empty(),
                format("%d sent", (int)platform.posted.size()));
        }
        _putenv_s("HBP_TEST_REPORT_SITE", "none");
        sys::removeTree(root + "/atmosphere");
        writeText(settingsPath, settingsWere);
        sys::removeFile(root + "/config/settings.txt");
    }

    // An official update installed over a game this rebuilt that's in it already only stops it starting, which the list says, and nothing is
    // made of it.
    if (nsps.size() >= 4) {
        auto console = freshConsole();
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            if (app.snapshot().tickHint == "Tick all")
                person.press(ButtonY);
            person.press(ButtonX);
            person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 30);
            person.press(ButtonA);
            person.waitUntilIdle();
            person.press(ButtonA);
            person.waitUntilIdle();
        }
        // The update by itself, as an installer would put it in.
        console->install({nsps[3]}, Storage::Sd);
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            int flagged = 0, ticked = 0;
            std::string seen;
            for (auto& row : app.snapshot().rows) {
                seen += row.name + ": " + row.note + "; ";
                if (row.note.find("no newer than what's in it") != std::string::npos) {
                    flagged++;
                    ticked += row.ticked ? 1 : 0;
                }
            }
            person.shot("real_updated_over_rebuilt");
            report("an official update installed over a game this rebuilt, that's in it already, is said to stop it starting, on that game alone", flagged == 1 && ticked == 0, seen);
        }
    }

    // A newer official update installed over a game this compressed, the way any installer puts one in, stops it starting until compressing
    // the game makes it part of it, from the copy, without the original game's file: one title at the update's version, with nothing of the
    // update left beside it, whose game data is byte for byte what the original game and the update make.
    if (nsps.size() >= 4) {
        sys::renameFile(root + "/sd/games", root + "/games kept aside");
        if (sys::isDirectory(root + "/output"))
            sys::renameFile(root + "/output", root + "/output kept aside");
        sys::removeTree(root + "/console11");
        sys::makeDirectories(root + "/sd/games");
        wipeData(root);
        std::string settings = "welcomed=1\nproblemsasked=1\nmeasureall=0\ndeleteasked=1\ncheckwritten=1\nhidecompressed=0\nwritelog=1\nfrompcnetwork=0\nfrompccable=0\n";
        writeText(root + "/data/settings.txt", settings + "minimumsaving=0\n");
        SimConsole console(root + "/console11", keyset);
        console.install({nsps[2]}, Storage::Sd);
        u64 application = console.listApplications().at(0);
        std::string key = format("%016llx", (unsigned long long)application);
        auto installedRow = [&](App& app, App::FullRow& out) {
            for (auto& row : app.snapshot().all) {
                if (row.tab == 0 && row.key == key) {
                    out = row;
                    return true;
                }
            }
            return false;
        };
        auto versions = [&](u32& base, u32& update, bool& patchLeft) {
            base = update = 0;
            patchLeft = false;
            for (auto& meta : console.describe(application).metas) {
                if (meta.key.type == Cnmt::MetaApplication)
                    base = meta.key.version;
                if (meta.key.type == Cnmt::MetaPatch) {
                    patchLeft = true;
                    update = meta.key.version;
                }
            }
        };
        {
            HostPlatform platform(root, keys, &console);
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            person.press(ButtonX);
            person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 30);
            person.press(ButtonA);
            person.waitUntilIdle();
            report("the base game alone gets compressed, before an update goes in over it", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle);
        }
        // The update by itself, as an installer would put it in.
        console.install({nsps[3]}, Storage::Sd);
        u32 copyVersion = 0, updateVersion = 0;
        bool patched = false;
        versions(copyVersion, updateVersion, patched);
        // However little making it part of it saves, it's to do: the game doesn't start as it is.
        writeText(root + "/data/settings.txt", settings + "minimumsaving=50\n");
        {
            HostPlatform platform(root, keys, &console);
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            App::FullRow before;
            bool found = installedRow(app, before);
            report("a newer update installed over a game this compressed is to do and ticked, saying it won't start until it's made part of it",
                patched && updateVersion > copyVersion && found && before.inToDo && before.ticked && !before.blocked && before.state == "Ready" && before.hasEstimate &&
                    before.second == "Won't start until its update is made part of it",
                format("v%u over v%u, ", updateVersion, copyVersion) + before.state + ", " + before.second + format(", ticked %d, saves %s", (int)before.ticked, before.saved.c_str()));
            person.shot("real_update_installed_over_copy");
            person.press(ButtonX);
            person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 30);
            person.press(ButtonA);
            person.waitUntilIdle();
            std::string said;
            for (auto& line : app.snapshot().results)
                said += line + "; ";
            u32 base = 0, update = 0;
            bool patchLeft = true;
            versions(base, update, patchLeft);
            report("and compressing it makes it one title at the update's version, with nothing of the update left beside it",
                app.snapshot().resultsTitle == "Finished" && base == updateVersion && !patchLeft && console.launchVersions[application] == updateVersion,
                app.snapshot().resultsTitle + ": " + said + format(" installed v%u, update left %d, launch version %u", base, (int)patchLeft, console.launchVersions[application]));
            person.shot("real_update_installed_over_copy_done");
            App::FullRow after;
            report("and the game's done", installedRow(app, after) && after.inDone && after.state == "Done", after.state + ", " + after.second);
        }
        {
            // What the original game and the update make, read from their files.
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(nsps[2]));
            set.add(std::make_shared<FileArchive>(nsps[3]));
            set.group();
            SourcePtr expected;
            for (auto& job : set.jobs) {
                for (auto& content : job.contents) {
                    SectionSource source;
                    for (int i = 0; i < NcaFile::SectionCount && !expected; i++) {
                        if (content.type == Cnmt::ContentTypeProgram && job.update && content.sectionAt(i, source) && source.section->fsType() == NcaFsSection::FsRomFs)
                            expected = source.owner->openRomFsImage(*source.section, source.original);
                    }
                }
            }
            // What the console has now.
            SourcePtr installed;
            TicketStore none(keyset);
            std::shared_ptr<NcaFile> program;
            for (auto& meta : console.describe(application).metas) {
                for (auto& content : meta.contents) {
                    if (meta.key.type != Cnmt::MetaApplication || content.type != Cnmt::ContentTypeProgram)
                        continue;
                    program = std::make_shared<NcaFile>(console.openContent(meta.storage, content.id, content.size), content.id.hex() + ".nca", keyset, none);
                    for (int i = 0; i < NcaFile::SectionCount && !installed; i++) {
                        if (program->section(i) && program->section(i)->fsType() == NcaFsSection::FsRomFs)
                            installed = program->openRomFsImage(*program->section(i), nullptr);
                    }
                }
            }
            i64 differ = -1;
            if (expected && installed && expected->length() == installed->length()) {
                Bytes a(1 << 20), b(1 << 20);
                for (i64 at = 0; at < expected->length() && differ < 0; at += (i64)a.size()) {
                    size_t n = (size_t)std::min<i64>((i64)a.size(), expected->length() - at);
                    expected->read(at, a.data(), n);
                    installed->read(at, b.data(), n);
                    if (memcmp(a.data(), b.data(), n) != 0)
                        differ = at;
                }
            }
            report("and its game data is byte for byte what the original game and the update make", expected && installed && expected->length() == installed->length() && differ < 0,
                format("%lld and %lld bytes, first difference in the MB at %lld", (long long)(expected ? expected->length() : -1), (long long)(installed ? installed->length() : -1), (long long)differ));
        }
        healthy(console, "after an update installed over a compressed game was made part of it");
        sys::removeTree(root + "/sd/games");
        sys::renameFile(root + "/games kept aside", root + "/sd/games");
        sys::removeTree(root + "/output");
        if (sys::isDirectory(root + "/output kept aside"))
            sys::renameFile(root + "/output kept aside", root + "/output");
        sys::removeTree(root + "/console11");
        startOver(root);
    }

    // An update file for a game that's installed goes in made part of it, compressed, without the original game's file: over the original
    // as installed, and over a copy of it this compressed.
    if (nsps.size() >= 4) {
        // What's on the card for the other tests is put aside meanwhile, and so is what they made, which has games extracted from
        // the same game in it, which would be the game.
        sys::renameFile(root + "/sd/games", root + "/games kept aside");
        if (sys::isDirectory(root + "/output"))
            sys::renameFile(root + "/output", root + "/output kept aside");
        for (bool compressedFirst : {false, true}) {
            std::string label = compressedFirst ? "over a compressed copy" : "over the original";
            sys::removeTree(root + "/console10");
            sys::removeTree(root + "/sd/games");
            sys::makeDirectories(root + "/sd/games");
            wipeData(root);
            writeText(root + "/data/settings.txt", "welcomed=1\nproblemsasked=1\nmeasureall=0\ndeleteasked=1\ninstallgamefiles=1\nminimumsaving=0\ncheckwritten=1\nhidecompressed=0\nmoresettings=1\nwritelog=1\nfrompcnetwork=0\nfrompccable=0\n");
            SimConsole console(root + "/console10", keyset);
            console.install({nsps[2]}, Storage::Sd);
            u64 application = console.listApplications().at(0);
            if (compressedFirst) {
                HostPlatform platform(root, keys, &console);
                App app(platform);
                app.start();
                Person person(app, shots);
                person.waitUntilIdle();
                person.press(ButtonX);
                person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 30);
                person.press(ButtonA);
                person.waitUntilIdle();
                report("the base game alone gets compressed first", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle);
            }
            {
                Bytes update = readWhole(nsps[3]);
                sys::File out(root + "/sd/games/the update.nsp", sys::File::Mode::Create);
                out.write(0, update.data(), update.size());
            }
            HostPlatform platform(root, keys, &console);
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToTab(person, app, 1);
            person.waitUntilIdle();
            std::string seen;
            bool listed = false;
            for (auto& row : app.snapshot().rows) {
                seen += row.name + ": " + row.detail + "; ";
                listed = listed || row.detail.find("for the installed game") != std::string::npos;
            }
            report("an update for an installed game is on Game files, " + label, listed, seen);
            person.shot(compressedFirst ? "real_update_over_copy" : "real_update_over_original");
            person.press(ButtonX);
            person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 30);
            person.press(ButtonA);
            person.waitUntilIdle();
            std::string said;
            for (auto& line : app.snapshot().results)
                said += line + "; ";
            InstalledGame game = console.describe(application);
            u32 baseVersion = 0;
            bool patchLeft = false;
            for (auto& meta : game.metas) {
                if (meta.key.type == Cnmt::MetaApplication)
                    baseVersion = meta.key.version;
                patchLeft = patchLeft || meta.key.type == Cnmt::MetaPatch;
            }
            report("and goes in made part of the game, " + label, app.snapshot().resultsTitle == "Finished" && baseVersion > 0 && !patchLeft,
                app.snapshot().resultsTitle + ": " + said + format(" installed v%u", baseVersion));
            healthy(console, "after an update went in " + label);
        }
        sys::removeTree(root + "/sd/games");
        sys::renameFile(root + "/games kept aside", root + "/sd/games");
        sys::removeTree(root + "/output");
        if (sys::isDirectory(root + "/output kept aside"))
            sys::renameFile(root + "/output kept aside", root + "/output");
        sys::removeTree(root + "/console10");
        startOver(root);
    }

    // A copy an older version made from a game that's only whole with its update kept saying so, and the console asks for an update and won't
    // start it. The overview says so, and fixing it takes that off its record and changes nothing else, while an original game that says the
    // same, which only its update would put right, is left alone.
    // In a function of its own, so the apps in it aren't on the stack for the whole of the test, which they'd overflow.
    if (nsps.size() >= 4) [&] {
        sys::renameFile(root + "/sd/games", root + "/games kept aside");
        if (sys::isDirectory(root + "/output"))
            sys::renameFile(root + "/output", root + "/output kept aside");
        sys::removeTree(root + "/console12");
        sys::makeDirectories(root + "/sd/games");
        wipeData(root);
        writeText(root + "/data/settings.txt", "welcomed=1\nproblemsasked=1\nmeasureall=0\ndeleteasked=1\nminimumsaving=0\ncheckwritten=1\nhidecompressed=0\nwritelog=1\nfrompcnetwork=0\nfrompccable=0\n");
        SimConsole console(root + "/console12", keyset);
        console.install({nsps[2]}, Storage::Sd);
        // Named and with their icons from the start, as the app keeps the name it first read for a game whose contents stay the same.
        appearance(console);
        u64 copy = console.listApplications().at(0);
        {
            HostPlatform platform(root, keys, &console);
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            person.press(ButtonX);
            person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 30);
            person.press(ButtonA);
            person.waitUntilIdle();
            report("a game gets compressed, to be given the mark an older version left", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle);
        }
        console.install({nsps[0]}, Storage::Sd);
        u64 original = 0;
        for (u64 application : console.listApplications())
            original = application != copy ? application : original;
        auto baseRecord = [&](u64 application, MetaKey& key) {
            for (auto& meta : console.describe(application).metas) {
                if (meta.key.type == Cnmt::MetaApplication) {
                    key = meta.key;
                    return console.getMeta(meta.storage, meta.key);
                }
            }
            return Bytes();
        };
        // As 1.4.3 left its copies of such a game, and as an original one that's only whole with its update has it.
        for (u64 application : {copy, original}) {
            MetaKey key;
            Bytes record = baseRecord(application, key);
            if (record.size() >= 8) {
                record[6] |= Cnmt::AttributeCompacted;
                console.setMeta(Storage::Sd, key, record);
            }
        }
        MetaKey copyKey;
        Bytes markedRecord = baseRecord(copy, copyKey);
        console.launchVersions.clear();
        {
            HostPlatform platform(root, keys, &console);
            App app(platform);
            app.start();
            Person person(app, shots);
            person.looksAtOverview = true;
            person.waitUntilIdle();
            auto wontStartLine = [&] {
                App::Snapshot now = app.snapshot();
                for (size_t i = 0; i < now.overviewLines.size(); i++) {
                    if (now.overviewLines[i].kind == "WontStart")
                        return (int)i;
                }
                return -1;
            };
            int line = wontStartLine();
            std::string lines;
            for (auto& seen : app.snapshot().overviewLines)
                lines += seen.kind + ": " + seen.text + "; ";
            report("the overview says the copy won't start, and not the original game", line >= 0 && app.snapshot().overviewLines[(size_t)line].text == "1 won't start as it is", lines);
            report("and with nothing to free, so does its headline", app.snapshot().headline == "1 game won't start as it is", app.snapshot().headline);
            person.shot("real_wont_start");
            // A tap puts the pointer on the line, and a second one presses it.
            App::Snapshot::Stop stop;
            for (auto& seen : app.snapshot().overviewStops)
                stop = seen.kind == "Line" && seen.line == line ? seen : stop;
            for (int tap = 0; tap < 2; tap++) {
                InputState touch;
                touch.touching = true;
                touch.touchX = stop.x + stop.w / 2;
                touch.touchY = stop.y + stop.h / 2;
                person.frame(touch);
                person.frame();
                person.settle(20);
            }
            App::Snapshot asked = app.snapshot();
            report("A on it says why the game won't start, and offers to fix it", asked.dialog.find("won't start as it is") != std::string::npos &&
                   asked.dialogText.find("1.4.3") != std::string::npos && asked.dialogText.find("Nothing else changes") != std::string::npos, asked.dialog + ": " + asked.dialogText);
            person.shot("real_wont_start_asked");
            // The pointer starts on Fix it.
            person.press(ButtonA);
            person.waitUntilIdle();
            std::string said;
            for (auto& result : app.snapshot().results)
                said += result + "; ";
            report("fixing it says so", app.snapshot().resultsTitle == "Finished" && said.find("Fixed") != std::string::npos, app.snapshot().resultsTitle + ": " + said);
            person.shot("real_wont_start_fixed");
            MetaKey key;
            Bytes fixedRecord = baseRecord(copy, key);
            Bytes expected = markedRecord;
            if (expected.size() >= 8)
                expected[6] &= (u8)~Cnmt::AttributeCompacted;
            report("its record no longer says it needs its update, and nothing else in it changed", markedRecord.size() >= 8 && fixedRecord == expected,
                   toHex(fixedRecord.data(), std::min<size_t>(fixedRecord.size(), 8)) + " for " + toHex(expected.data(), std::min<size_t>(expected.size(), 8)));
            report("the console is told the version there is the one to start", console.launchVersions.count(copy) && console.launchVersions[copy] == copyKey.version,
                   format("%d told", (int)console.launchVersions.size()));
            Bytes originalRecord = baseRecord(original, key);
            report("an original game that says it needs its update is left as it is", originalRecord.size() >= 8 && (originalRecord[6] & Cnmt::AttributeCompacted) && !console.launchVersions.count(original));
            person.press(ButtonA);
            person.settle();
            report("and the overview no longer says a game won't start", app.snapshot().overview && wontStartLine() < 0 && app.snapshot().headline.find("won't start") == std::string::npos, app.snapshot().headline);
        }
        // A copy made in place of a game an installer had let start on an older system asks, as 1.5.0 test 2 left it, for the system version
        // the game's files say, which this console doesn't have. Fix lowers it to the console's own, on the copy alone.
        {
            constexpr u32 Asks = 23u << 26, Console = 22u << 26 | 5u << 20;
            for (u64 application : {copy, original}) {
                MetaKey key;
                Bytes record = baseRecord(application, key);
                if (record.size() >= 8 + 0xC) {
                    wr32(record.data() + 16, Asks);
                    console.setMeta(Storage::Sd, key, record);
                }
            }
            MetaKey key;
            Bytes asking = baseRecord(copy, key);
            Bytes originalBefore = baseRecord(original, key);
            HostPlatform platform(root, keys, &console);
            platform.system = Console;
            App app(platform);
            app.start();
            Person person(app, shots);
            person.looksAtOverview = true;
            person.waitUntilIdle();
            int line = -1;
            for (size_t i = 0; i < app.snapshot().overviewLines.size(); i++)
                line = app.snapshot().overviewLines[i].kind == "WontStart" ? (int)i : line;
            report("a copy asking for a newer system version than the console's won't start, and the original asking the same isn't counted",
                   line >= 0 && app.snapshot().overviewLines[(size_t)line].text == "1 won't start as it is", line >= 0 ? app.snapshot().overviewLines[(size_t)line].text : "no line");
            App::Snapshot::Stop stop;
            for (auto& seen : app.snapshot().overviewStops)
                stop = seen.kind == "Line" && seen.line == line ? seen : stop;
            for (int tap = 0; tap < 2; tap++) {
                InputState touch;
                touch.touching = true;
                touch.touchX = stop.x + stop.w / 2;
                touch.touchY = stop.y + stop.h / 2;
                person.frame(touch);
                person.frame();
                person.settle(20);
            }
            App::Snapshot asked = app.snapshot();
            report("A on it says what the copy asks for and what the console has", asked.dialogText.find("23.0.0") != std::string::npos &&
                   asked.dialogText.find("22.5.0") != std::string::npos && asked.dialogText.find("1.4.3") == std::string::npos, asked.dialog + ": " + asked.dialogText);
            person.shot("real_asks_system_asked");
            person.press(ButtonA);
            person.waitUntilIdle();
            std::string said;
            for (auto& result : app.snapshot().results)
                said += result + "; ";
            report("fixing it says so", app.snapshot().resultsTitle == "Finished" && said.find("Fixed") != std::string::npos, app.snapshot().resultsTitle + ": " + said);
            Bytes fixedRecord = baseRecord(copy, key);
            Bytes expected = asking;
            if (expected.size() >= 8 + 0xC)
                wr32(expected.data() + 16, Console);
            report("its record asks for the console's own version, and nothing else in it changed", asking.size() >= 8 + 0xC && fixedRecord == expected,
                   fixedRecord.size() >= 20 ? format("asks %08x", rd32(fixedRecord.data() + 16)) : std::string("no record"));
            report("the original game asking the same is left as it is", baseRecord(original, key) == originalBefore);
            person.press(ButtonA);
            person.settle();
            report("and the overview no longer says a game won't start", app.snapshot().overview && app.snapshot().headline.find("won't start") == std::string::npos, app.snapshot().headline);
            // Back as it was, for what follows.
            for (u64 application : {copy, original}) {
                Bytes record = baseRecord(application, key);
                if (record.size() >= 8 + 0xC) {
                    wr32(record.data() + 16, 12u << 26);
                    console.setMeta(Storage::Sd, key, record);
                }
            }
        }
        // What the console wants of each game goes in the log as the app opens, as that's why a game asks for an update before it starts: a
        // compressed game it knows a newer version of, and any game it won't start as it is. (A game that isn't known to be compressed, which
        // it only knows a newer version of, is left out: the log test has that.)
        {
            MetaKey key;
            baseRecord(copy, key);
            u32 copyVersion = key.version;
            baseRecord(original, key);
            u32 originalVersion = key.version;
            std::string copyName;
            auto openAndClose = [&] {
                HostPlatform platform(root, keys, &console);
                App app(platform);
                app.start();
                Person person(app, shots);
                person.waitUntilIdle();
                for (auto& row : app.snapshot().all)
                    copyName = row.tab == 0 && row.key == format("%016llx", (unsigned long long)copy) ? row.name : copyName;
            };
            auto count = [](const std::string& text, const std::string& what) {
                int found = 0;
                for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1))
                    found++;
                return found;
            };
            auto lineWith = [](const std::string& text, const std::string& what) {
                size_t at = text.find(what);
                if (at == std::string::npos)
                    return std::string();
                size_t start = text.rfind('\n', at);
                start = start == std::string::npos ? 0 : start + 1;
                return text.substr(start, text.find('\n', at) - start);
            };
            const std::string offers = "offers a newer version before starting it", wontStart = "won't start it as it is";
            console.versionList[copy] = {copyVersion + 0x10000, 0};
            openAndClose();
            std::string log = readText(root + "/data/log.txt"), line = lineWith(log, offers);
            report("the log names a compressed game the console offers a newer version of, by its name and title ID, with the versions",
                   count(log, offers) == 1 && count(log, wontStart) == 0 && !copyName.empty() &&
                   line.find(copyName + format(" [%016llX]: v%u installed", (unsigned long long)copy, copyVersion)) != std::string::npos &&
                   line.find(format("v%u as the newest", copyVersion + 0x10000)) != std::string::npos,
                   line.empty() ? format("%d lines", count(log, offers)) : line);
            console.launchVersions[original] = originalVersion + 0x10000;
            openAndClose();
            log = readText(root + "/data/log.txt");
            line = lineWith(log, wontStart);
            report("and a game the console won't start as it is, compressed or not", count(log, wontStart) == 1 &&
                   line.find(format("[%016llX]: v%u installed", (unsigned long long)original, originalVersion)) != std::string::npos &&
                   line.find(format("it wants v%u to start it", originalVersion + 0x10000)) != std::string::npos,
                   line.empty() ? std::string("not in the log") : line);
            console.versionList.clear();
            console.launchVersions.erase(original);
        }
        healthy(console, "after fixing a copy that said it needs its update");
        sys::removeTree(root + "/sd/games");
        sys::renameFile(root + "/games kept aside", root + "/sd/games");
        sys::removeTree(root + "/output");
        if (sys::isDirectory(root + "/output kept aside"))
            sys::renameFile(root + "/output kept aside", root + "/output");
        sys::removeTree(root + "/console12");
        startOver(root);
    }();

    if (nsps.size() >= 3)
        gameCardTest();

    if (nsps.size() >= 2)
        lookingInside();

    // A game that reads back damaged when it's read in full is remembered as that, rather than read in full again, for minutes, each time the
    // app opens: until it's analyzed again by hand, which reads it once more.
    // In a function of its own, so the apps in it aren't on the stack for the whole of the test, which they'd overflow.
    if (nsps.size() >= 4) [&] {
        sys::renameFile(root + "/sd/games", root + "/games kept aside");
        if (sys::isDirectory(root + "/output"))
            sys::renameFile(root + "/output", root + "/output kept aside");
        sys::removeTree(root + "/console13");
        sys::makeDirectories(root + "/sd/games");
        wipeData(root);
        writeText(root + "/data/settings.txt", "welcomed=1\nproblemsasked=1\nmeasureall=1\ndeleteasked=1\nminimumsaving=0\nhidecompressed=0\nwritelog=1\nfrompcnetwork=0\nfrompccable=0\n");
        SimConsole console(root + "/console13", keyset);
        console.install({nsps[2]}, Storage::Sd);
        appearance(console);
        u64 application = console.listApplications().at(0);
        std::string key = format("%016llx", (unsigned long long)application);
        // A byte in the middle of its game data, which a first look reads past and reading it in full doesn't.
        auto flipProgram = [&] {
            for (auto& meta : console.describe(application).metas) {
                for (auto& content : meta.contents) {
                    if (meta.key.type != Cnmt::MetaApplication || content.type != Cnmt::ContentTypeProgram)
                        continue;
                    sys::File file(console.contentPath(meta.storage, content.id), sys::File::Mode::Update);
                    u8 byte = 0;
                    file.read(content.size / 2, &byte, 1);
                    byte ^= 0x40;
                    file.write(content.size / 2, &byte, 1);
                }
            }
        };
        auto rowOf = [&](App& app) {
            for (auto& row : app.snapshot().all) {
                if (row.tab == 0 && row.key == key)
                    return row;
            }
            return App::FullRow();
        };
        flipProgram();
        {
            HostPlatform platform(root, keys, &console);
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            App::FullRow row = rowOf(app);
            report("a game that reads back damaged when it's read in full says so", row.state == "Failed" && !row.ticked, row.state + ": " + row.status);
            report("in plain words on the list", row.second == "Damaged: dump or download it again.", row.second);
        }
        // Put right, as reinstalling the same update would, which doesn't change what the game is made of.
        flipProgram();
        {
            HostPlatform platform(root, keys, &console);
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            App::FullRow row = rowOf(app);
            report("the next time the app opens, that's remembered rather than read in full again", row.state == "Failed" && !row.hasEstimate, row.state + ": " + row.status);
            person.shot("real_damaged_remembered");
            chooseInMenu(person, app, "Analyze this game again");
            person.waitUntilIdle();
            row = rowOf(app);
            report("analyzed again by hand, it's read once more, and comes out right now that it reads right", row.state != "Failed" && row.hasEstimate && row.measured,
                   row.state + ": " + row.status);
        }
        healthy(console, "after a game that read back damaged was put right");
        sys::removeTree(root + "/sd/games");
        sys::renameFile(root + "/games kept aside", root + "/sd/games");
        sys::removeTree(root + "/output");
        if (sys::isDirectory(root + "/output kept aside"))
            sys::renameFile(root + "/output kept aside", root + "/output");
        sys::removeTree(root + "/console13");
        startOver(root);
    }();

    // Looking in one folder, a game from it compressed into the NSP folder, rather than beside it, goes with its original the next time the app
    // opens, as one saved beside it does: it isn't to do again.
    // In a function of its own, so the apps in it aren't on the stack for the whole of the test, which they'd overflow.
    if (nsps.size() >= 4) [&] {
        sys::renameFile(root + "/sd/games", root + "/games kept aside");
        if (sys::isDirectory(root + "/output"))
            sys::renameFile(root + "/output", root + "/output kept aside");
        std::string originals = root + "/sd/games/originals", compressed = root + "/sd/compressed";
        sys::makeDirectories(originals);
        sys::removeTree(compressed);
        {
            Bytes bytes = readWhole(nsps[2]);
            sys::File out(originals + "/One Folder Test.nsp", sys::File::Mode::Create);
            out.write(0, bytes.data(), bytes.size());
        }
        wipeData(root);
        writeText(root + "/data/settings.txt", "welcomed=1\nproblemsasked=1\nmeasureall=0\ndeleteasked=1\ninstallgamefiles=0\nminimumsaving=0\ncheckwritten=0\nhidecompressed=0\nwritelog=1\n"
                                               "frompcnetwork=0\nfrompccable=0\nfilesfolder=" + originals + "\nnspfolder=" + compressed + "\n");
        sys::removeTree(root + "/console14");
        SimConsole console(root + "/console14", keyset);
        auto copies = [&] {
            int count = 0;
            for (auto& entry : sys::list(compressed))
                count += endsWithNoCase(entry.name, "[LZ4].nsp") ? 1 : 0;
            return count;
        };
        auto fileRow = [&](App& app) {
            for (auto& row : app.snapshot().all) {
                if (row.tab == 1)
                    return row;
            }
            return App::FullRow();
        };
        {
            HostPlatform platform(root, keys, &console);
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToTab(person, app, 1);
            person.waitUntilIdle();
            person.press(ButtonX);
            person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 60);
            person.press(ButtonA);
            person.waitUntilIdle();
            report("looking in one folder, a game from it is compressed into the NSP folder", app.snapshot().resultsTitle == "Finished" && copies() == 1,
                   format("%s, %d copies", app.snapshot().resultsTitle.c_str(), copies()));
        }
        {
            HostPlatform platform(root, keys, &console);
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToTab(person, app, 1);
            person.waitUntilIdle();
            App::FullRow row = fileRow(app);
            report("the next time, it's found with its original, done rather than to do again", row.inDone && !row.inToDo && row.mainFile.find("[LZ4]") != std::string::npos,
                   format("%s: %s, to do %d, done %d", row.mainFile.c_str(), row.status.c_str(), row.inToDo ? 1 : 0, row.inDone ? 1 : 0));
            person.shot("real_one_folder_copy_found");
        }
        sys::removeTree(compressed);
        sys::removeTree(root + "/sd/games");
        sys::renameFile(root + "/games kept aside", root + "/sd/games");
        sys::removeTree(root + "/output");
        if (sys::isDirectory(root + "/output kept aside"))
            sys::renameFile(root + "/output kept aside", root + "/output");
        sys::removeTree(root + "/console14");
        startOver(root);
    }();

    // Changing compression works out every game again, one unticked by hand too, and one done already: not only what's ticked.
    {
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        person.press(ButtonA);
        report("a game can be unticked before compression is changed", !app.snapshot().rows.empty() && !app.snapshot().rows[0].ticked);
        goToOption(person, app, "Compression");
        person.press(ButtonRight);
        person.press(ButtonB);
        person.waitUntilIdle();
        std::string without;
        for (auto& row : app.snapshot().rows) {
            if (!row.hasEstimate)
                without += row.name + "; ";
        }
        report("turning compression off works out every game again, ticked or not", !app.snapshot().rows.empty() && without.empty(), without);
        goToOption(person, app, "Compression");
        person.press(ButtonRight);
        person.press(ButtonB);
        person.waitUntilIdle();
    }

    // Checking for damage.
    {
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        auto checkAll = [&] {
            chooseInMenu(person, app, "Check the ticked");
            person.waitUntilIdle();
        };
        checkAll();
        person.shot("real_checked");
        report("whole games get reported as whole", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        person.press(ButtonA);

        console->setCardCorrupts(true);
        checkAll();
        console->setCardCorrupts(false);
        person.shot("real_checked_damaged");
        report("damage gets found", app.snapshot().resultsTitle.find("problems") != std::string::npos, app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        person.press(ButtonA);
    }

    // An extracted game gets built into an NSP again.
    {
        auto console = freshConsole();
        sys::removeTree(root + "/sd/games");
        sys::removeTree(root + "/output");
        sys::removeFile(root + "/data/list.cache");
        std::string extractedInto = root + "/output/extracted";
        {
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(nsps.size() > 2 ? nsps[2] : nsps[0]));
            set.group();
            extractTitle(set.jobs.at(0), set, extractedInto);
        }
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        person.press(ButtonR);
        person.waitUntilIdle();
        report("an extracted game is one game on the list of game files, not the files inside it", app.snapshot().rows.size() == 1, format("%d", (int)app.snapshot().rows.size()));
        person.waitUntilIdle();
        person.settle();
        person.shot("real_extracted_listed");
        App::Snapshot snapshot = app.snapshot();
        report("the extracted game is listed", snapshot.rows.size() == 1 && snapshot.rows[0].hasEstimate, format("%d", (int)snapshot.rows.size()));
        person.press(ButtonX);
        person.shot("real_extracted_confirm");
        person.press(ButtonA);
        person.waitUntilIdle();
        person.shot("real_extracted_built");
        report("building it finishes", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        person.press(ButtonA);

        std::string built;
        for (auto& entry : sys::list(root + "/output")) {
            if (endsWithNoCase(entry.name, ".nsp"))
                built = root + "/output/" + entry.name;
        }
        report("and there's an NSP of it", !built.empty());
        if (!built.empty()) {
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(built));
            set.group();
            report("that opens as a game", set.jobs.size() == 1 && set.problems.empty());
        }
        person.waitUntilIdle();
        person.settle();
        report("which doesn't go on the list beside it", app.snapshot().rows.size() == 1, format("%d", (int)app.snapshot().rows.size()));

        // A folder chosen with the file explorer, on the USB drive that's just been plugged in.
        std::string onUsb = root + "/usb/my games";
        extractedInto = onUsb;
        {
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(nsps.size() > 2 ? nsps[2] : nsps[0]));
            set.group();
            extractTitle(set.jobs.at(0), set, extractedInto);
        }
        platform.usb = true;
        platform.usbChanged = true;
        person.settle();
        report("a USB drive being plugged in gets noticed", app.snapshot().toast == "USB drive found", app.snapshot().toast);
        person.press(ButtonStickL);
        report("the left stick does nothing now", app.snapshot().screen == "Main", app.snapshot().screen);
        person.press(ButtonMinus);
        report("- does nothing on the lists any more", app.snapshot().screen == "Main" && app.snapshot().menu.empty(), app.snapshot().screen);
        goToMenuRow(person, app, "Where to look");
        person.press(ButtonA);
        // On the list of game files, it says where to look, which can be one folder chosen with the file explorer.
        auto where = app.snapshot().menu;
        std::string listedWhere;
        for (auto& item : where)
            listedWhere += item + "; ";
        report("the + menu says where to look",listedWhere.find("The SD card: Shown") != std::string::npos && listedWhere.find("A PC on the USB cable") != std::string::npos, listedWhere);
        person.shot("real_where_to_look");
        // To the folder chooser, wherever it is on the page: the game card, and a PC on both, come before it when they're there.
        for (int i = 0; i < (int)where.size(); i++) {
            if (!containsNoCase(where[(size_t)i], "look in one folder"))
                continue;
            while (app.snapshot().menuFocus != i)
                person.press(app.snapshot().menuFocus < i ? ButtonDown : ButtonUp);
            break;
        }
        person.press(ButtonA);
        report("and one folder can be chosen with the file explorer", app.snapshot().screen == "Browse", app.snapshot().screen);
        person.shot("real_browse_folder");
        while (!app.snapshot().browsing.empty())
            person.press(ButtonB);
        int drivesShown = 0;
        for (auto& entry : app.snapshot().browseEntries)
            drivesShown += entry.rfind("PC", 0) == 0 ? 0 : 1;
        report("the drives are all there to start from", drivesShown == 2, format("%d", (int)app.snapshot().browseEntries.size()));
        person.shot("real_browse_drives");
        person.press(ButtonDown);
        person.press(ButtonA);
        report("going into the USB drive", app.snapshot().browseEntries.size() == 1 && app.snapshot().browseEntries[0] == "my games", app.snapshot().browseEntries.empty() ? "" : app.snapshot().browseEntries[0]);
        person.press(ButtonA);
        App::Snapshot inside = app.snapshot();
        report("and folders holding a game are marked", inside.browseEntries.size() == 1 && inside.browseEntries[0].find("Extracted game") != std::string::npos, inside.browseEntries.empty() ? "" : inside.browseEntries[0]);
        person.shot("real_browse_game");
        person.press(ButtonX);
        report("choosing it goes back", app.snapshot().screen == "Main", app.snapshot().screen);
        person.waitUntilIdle();
        person.settle();
        snapshot = app.snapshot();
        report("and lists what's in it", snapshot.rows.size() == 1 && !snapshot.rows.empty(), format("%d", (int)snapshot.rows.size()));
        person.shot("real_folder_on_usb");
        report("the choice is kept", readText(root + "/data/settings.txt").find("filesfolder=" + onUsb) != std::string::npos);

        // NSPs going onto the USB drive too.
        goToOption(person, app, "NSP files go in");
        person.press(ButtonA);
        report("the NSP folder gets chosen in the file explorer", app.snapshot().screen == "Browse", app.snapshot().screen);
        while (!app.snapshot().browsing.empty())
            person.press(ButtonB);
        person.press(ButtonDown);
        person.press(ButtonA);
        person.press(ButtonX);
        report("and it goes back to the options", app.snapshot().screen == "Options", app.snapshot().screen);
        person.shot("real_options_folders");
        goToTab(person, app, 1);
        person.waitUntilIdle();
        person.press(ButtonX);
        report("the room gets looked at on that drive", app.snapshot().dialogText.find(root + "/usb") != std::string::npos, app.snapshot().dialogText);
        person.press(ButtonA);
        person.waitUntilIdle();
        report("building it there finishes", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        person.press(ButtonA);
        bool onDrive = false;
        for (auto& entry : sys::list(root + "/usb"))
            onDrive = onDrive || endsWithNoCase(entry.name, ".nsp");
        report("and the NSP is on the drive", onDrive);

        platform.usb = false;
        platform.usbChanged = true;
        person.settle();
        report("a drive being taken out gets noticed", app.snapshot().toast == "USB drive taken out", app.snapshot().toast);
        person.waitUntilIdle();
        sys::removeTree(root + "/usb");
        startOver(root);
        sys::removeFile(root + "/data/list.cache");
    }

    // With compression off it's a repack, called what it is, which can go in place of what's installed.
    {
        auto console = freshConsole();
        i64 before = 0;
        for (u64 id : console->listApplications())
            before += console->describe(id).totalBytes();
        sys::removeTree(root + "/output");
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        goToOption(person, app, "Compression");
        auto shown = [&](const char* label) {
            for (auto& option : app.snapshot().options) {
                if (option.rfind(std::string(label) + ":", 0) == 0)
                    return true;
            }
            return false;
        };
        report("with compression on, its own settings are there", shown("Minimum saving") && shown("Compression level"));
        report("there's no block size to choose", !shown("Block size"));
        person.press(ButtonRight);
        person.shot("real_options_no_compression");
        App::Snapshot off = app.snapshot();
        report("with it off, they're gone, and the focus stays on it", !shown("Minimum saving") && !shown("Compression level") &&
            off.optionFocus >= 0 && off.optionFocus < (int)off.options.size() && off.options[(size_t)off.optionFocus].rfind("Compression:", 0) == 0,
            off.optionFocus >= 0 && off.optionFocus < (int)off.options.size() ? off.options[(size_t)off.optionFocus] : "nothing");
        goToTab(person, app, 0);
        person.waitUntilIdle();
        person.settle();
        person.shot("real_repack_analyzed");
        person.press(ButtonX);
        report("repacking asks first", app.snapshot().dialog.find("Repack") == 0, app.snapshot().dialog);
        report("and says what happens to the game", app.snapshot().dialogText.find("replaced by a repacked copy") != std::string::npos, app.snapshot().dialogText);
        person.shot("real_repack_replace_confirm");
        person.press(ButtonA);
        person.waitUntilIdle();
        report("repacking what's installed finishes", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        for (auto& line : app.snapshot().results)
            printf("  %s\n", line.c_str());
        person.shot("real_repack_replaced");
        person.press(ButtonA);
        healthy(*console, "after repacking what's installed");
        i64 after = 0;
        for (u64 id : console->listApplications())
            after += console->describe(id).totalBytes();
        printf("  %lld bytes before, %lld after\n", (long long)before, (long long)after);
        report("a game with an update comes out smaller", after < before);
        std::string log = readText(root + "/data/log.txt");
        report("the console's check is what says it takes sigpatches", log.find("files of the test copy back") != std::string::npos);
    }

    // Only saving an NSP leaves what's installed as it is.
    {
        auto console = freshConsole();
        i64 before = 0;
        for (u64 id : console->listApplications())
            before += console->describe(id).totalBytes();
        sys::removeTree(root + "/output");
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        goToOption(person, app, "Compression");
        person.press(ButtonRight);
        goToOption(person, app, "Installed games");
        person.press(ButtonRight);
        goToTab(person, app, 0);
        person.waitUntilIdle();
        person.settle();
        report("the button is called Repack", app.snapshot().actionWord == "Repack", app.snapshot().actionWord);
        person.shot("real_repack_list");
        person.press(ButtonX);
        report("and so is what it asks", app.snapshot().dialog.find("Repack") == 0, app.snapshot().dialog);
        person.shot("real_repack_confirm");
        person.press(ButtonA);
        std::string title;
        person.waitFor([&] { return app.snapshot().screen == "Job"; }, 30);
        person.shot("real_repacking");
        person.waitUntilIdle();
        report("repacking finishes", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        report("and calls it that", app.snapshot().resultsDetail.find("repacked") != std::string::npos, app.snapshot().resultsDetail);
        person.shot("real_repacked");
        person.press(ButtonA);
        i64 after = 0;
        for (u64 id : console->listApplications())
            after += console->describe(id).totalBytes();
        report("what's installed isn't touched", before == after);
        std::string repacked;
        for (auto& entry : sys::list(root + "/output")) {
            if (endsWithNoCase(entry.name, ".nsp"))
                repacked = root + "/output/" + entry.name;
        }
        report("and there's an NSP", !repacked.empty() && repacked.find("[LZ4]") == std::string::npos, repacked);
        if (!repacked.empty()) {
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(repacked));
            set.group();
            // With Game and DLC in one file on, as it starts, the game's file holds every DLC installed with it.
            int games = 0, dlcs = 0, installed = 0;
            u64 gameId = 0;
            for (auto& job : set.jobs) {
                if (job.isBaseGame()) {
                    games++;
                    gameId = job.main->meta.titleId;
                }
            }
            for (auto& job : set.jobs)
                dlcs += job.main->meta.type == Cnmt::MetaAddOnContent && job.main->meta.linkedId() == gameId;
            for (auto& meta : console->describe(gameId).metas)
                installed += meta.key.type == Cnmt::MetaAddOnContent;
            report("that opens as the game and the DLC installed with it", set.problems.empty() && games == 1 && dlcs == installed &&
                   (int)set.jobs.size() == 1 + dlcs, format("%d jobs, %d game, %d DLC of it, %d installed, %d problems", (int)set.jobs.size(), games,
                   dlcs, installed, (int)set.problems.size()));
        }
        std::string log = readText(root + "/data/log.txt");
        report("with no need for the console's check", log.find("files of the test copy back") == std::string::npos);
        startOver(root);
        sys::removeFile(root + "/data/list.cache");
    }

    // With compression off, a game in one piece has nothing to gain from a repack, whether it was repacked here or never had an update, so
    // once repacked, a game is still under Done when the app opens again. Only what had been done since it opened used to be.
    {
        auto console = freshConsole();
        writeText(root + "/data/settings.txt", readText(root + "/data/settings.txt") + "compress=0\n");
        auto count = [](App& app, int& toDo, int& done, std::string& seen) {
            toDo = done = 0;
            seen.clear();
            for (auto& row : app.snapshot().all) {
                if (row.tab != 0)
                    continue;
                toDo += row.inToDo ? 1 : 0;
                done += row.inDone ? 1 : 0;
                seen += row.name + ": " + row.second + (row.ticked ? ", ticked" : "") + "; ";
            }
        };
        int toDo = 0, done = 0;
        std::string seen;
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToTab(person, app, 0);
            person.waitUntilIdle();
            person.settle();
            count(app, toDo, done, seen);
            report("with compression off, installed games with their update beside them are to do", toDo == 2 && done == 0, seen);
            person.press(ButtonX);
            person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 60);
            report("and repacking them is asked about", app.snapshot().dialog.find("Repack") == 0, app.snapshot().dialog);
            person.press(ButtonA);
            person.waitUntilIdle();
            if (app.snapshot().screen == "Results")
                person.press(ButtonA);
            person.settle();
            count(app, toDo, done, seen);
            report("once repacked, they're done", toDo == 0 && done == 2, seen);
        }
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToTab(person, app, 0);
            person.waitUntilIdle();
            person.settle();
            count(app, toDo, done, seen);
            report("and when the app opens again, they're still done, in one piece with nothing to gain", toDo == 0 && done == 2 && seen.find("no update") != std::string::npos, seen);
            person.shot("real_repacked_reopened");
        }
        healthy(*console, "after repacking with compression off and opening again");
        startOver(root);
        sys::removeFile(root + "/data/list.cache");
    }

    // With compression off, a game with no update has nothing to gain: it starts unticked, under Done, saying why. Ticked by hand, it may be
    // for something else, so it's asked about.
    {
        const std::string& alone = nsps.size() > 2 ? nsps[2] : nsps[0];
        sys::removeTree(root + "/sd/games");
        sys::makeDirectories(root + "/sd/games");
        std::string copied = root + "/sd/games/alone.nsp";
        {
            Bytes bytes = readWhole(alone);
            sys::File out(copied, sys::File::Mode::Create);
            out.write(0, bytes.data(), bytes.size());
        }
        sys::removeTree(root + "/output");
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        goToOption(person, app, "Compression");
        person.press(ButtonRight);
        goToTab(person, app, 1);
        person.waitUntilIdle();
        person.settle();
        App::FullRow lone;
        for (auto& row : app.snapshot().all) {
            if (row.tab == 1 && row.mainFile == copied)
                lone = row;
        }
        // The row says where the file is, so it only has room for the start of why: the rest is in its details.
        report("with compression off, a game with no update starts unticked, under Done, saying why", lone.mainFile == copied && !lone.ticked && lone.inDone && !lone.inToDo &&
            lone.second.find("Nothing to gain") == 0, format("ticked %d, to do %d, done %d: %s", lone.ticked ? 1 : 0, lone.inToDo ? 1 : 0, lone.inDone ? 1 : 0, lone.second.c_str()));
        // It starts on Everything, as games that are done aren't hidden here, and Done is just to the left of it.
        std::string from = app.snapshot().views[1];
        person.press(ButtonLeft);
        std::string went = app.snapshot().views[1];
        person.press(ButtonRight);
        report("and Done is there to go to, with compression off too", from == "Everything" && went == "Done" && app.snapshot().views[1] == "Everything", from + " -> " + went);
        // With nothing saved to go by, Y only goes between all and none, and four times round leaves it as it was.
        std::string offered;
        for (int i = 0; i < 4; i++) {
            offered += app.snapshot().tickHint + "; ";
            person.press(ButtonY);
        }
        report("with compression off, Y doesn't offer what's worth it", offered.find("Worth it") == std::string::npos && !app.snapshot().rows.empty() && !app.snapshot().rows[0].ticked, offered);
        // Ticked by hand, then.
        person.press(ButtonA);
        report("it can be ticked by hand", !app.snapshot().rows.empty() && app.snapshot().rows[0].ticked);
        person.press(ButtonX);
        App::Snapshot asked = app.snapshot();
        report("a game with no update gets asked about", asked.dialog.find("no update") != std::string::npos, asked.dialog);
        person.shot("real_no_update_question");
        person.press(ButtonB);
        report("cancelling leaves the ticks as they were", !app.snapshot().rows.empty() && app.snapshot().rows[0].ticked && app.snapshot().dialog.empty(), app.snapshot().dialog);
        person.press(ButtonX);
        person.press(ButtonA);
        report("unticking it leaves nothing to repack", app.snapshot().dialog.find("Nothing left") != std::string::npos && !app.snapshot().rows[0].ticked, app.snapshot().dialog);
        person.press(ButtonA);
        person.press(ButtonY);
        person.press(ButtonX);
        person.press(ButtonRight);
        person.press(ButtonA);
        report("it can be repacked anyway", app.snapshot().dialog.find("Repack this") == 0, app.snapshot().dialog);
        person.press(ButtonB);
        report("and installing it doesn't get asked about at all", chooseInMenu(person, app, "Install the ticked") && app.snapshot().dialog.find("Install this") == 0, app.snapshot().dialog);
        person.press(ButtonB);
        startOver(root);
        sys::removeFile(root + "/data/list.cache");
    }

    // A game, its update and its DLC are one row, and so are copies of them. A compressed copy as new as what it was made from takes their place.
    if (nsps.size() > 4) {
        std::string games = root + "/sd/games";
        auto copyTo = [&](const std::string& from, const std::string& to) {
            Bytes bytes = readWhole(from);
            sys::File out(to, sys::File::Mode::Create);
            out.write(0, bytes.data(), bytes.size());
        };
        sys::removeTree(games);
        sys::makeDirectories(games + "/copy");
        copyTo(nsps[2], games + "/base.nsp");
        copyTo(nsps[3], games + "/update.nsp");
        copyTo(nsps[4], games + "/dlc.nsp");
        copyTo(nsps[2], games + "/copy/base.nsp");
        {
            wipeData(root);
            auto console = freshConsole();
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToTab(person, app, 1);
            person.waitUntilIdle();
            person.settle();
            auto rows = app.snapshot().rows;
            report("a game, its update and its DLC are one row", rows.size() == 1 && rows[0].detail.find("1 DLC") != std::string::npos && rows[0].detail.find("update") != std::string::npos,
                   rows.empty() ? format("%d rows", (int)rows.size()) : format("%d rows: %s", (int)rows.size(), rows[0].detail.c_str()));
            report("and the other copy is noted on it", !rows.empty() && rows[0].notes == 1, rows.empty() ? "" : format("%d", rows[0].notes));
            person.shot("real_game_with_dlc");
            // What's noted on a game is in its details, with the rest of what there is to know about it.
            bool opened = chooseInMenu(person, app, "Details") && person.waitFor([&] { return app.snapshot().insideReady; }, 60);
            std::string noted;
            for (auto& note : app.snapshot().detailsNotes)
                noted += note + "; ";
            report("what's noted is in its details", opened && noted.find("Another copy") != std::string::npos, noted);
            person.press(ButtonB);
        }

        sys::removeTree(games);
        sys::makeDirectories(games);
        copyTo(nsps[2], games + "/base.nsp");
        {
            // What this tool makes of it, as new as it is.
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(games + "/base.nsp"));
            set.group();
            ConvertOptions options;
            std::string made = games + "/" + outputFileName(set.jobs.at(0), set, options);
            NspOutput nsp(made, (int)set.jobs.at(0).contents.size(), 0, true);
            TitleConverter(options, keyset).run(set.jobs.at(0), nsp);
        }
        {
            wipeData(root);
            auto console = freshConsole();
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToTab(person, app, 1);
            person.waitUntilIdle();
            person.settle();
            report("a compressed copy as new takes the original's place", app.snapshot().rows.size() == 1 && app.snapshot().rows[0].compressed && app.snapshot().rows[0].notes == 1);
            for (int i = 0; i < 3 && app.snapshot().views[1] != "To do"; i++)
                person.press(ButtonLeft);
            person.settle();
            report("and showing what's to do leaves it off", app.snapshot().rows.empty() && app.snapshot().compressedHidden == 1,
                   [&] {
                       std::string what = format("%d rows, %d hidden", (int)app.snapshot().rows.size(), app.snapshot().compressedHidden);
                       for (auto& row : app.snapshot().rows)
                           what += format("; %s / %s / compressed %d / %s", row.name.c_str(), row.detail.c_str(), row.compressed ? 1 : 0, row.note.c_str());
                       for (auto& entry : sys::list(games))
                           what += "; file " + entry.name;
                       return what;
                   }());
        }
        // What comes next goes on from the game left there before.
        sys::removeTree(games);
        sys::makeDirectories(games);
        copyTo(nsps[2], games + "/alone.nsp");
        startOver(root);
        sys::removeFile(root + "/data/list.cache");
    }

    // Installing whatever's ticked from the menu, as it is or compressed, from a file or from an extracted game.
    {
        std::string games = root + "/sd/games";
        sys::removeTree(root + "/console4");
        wipeData(root);
        sys::removeTree(root + "/output");
        SimConsole console(root + "/console4", keyset);
        HostPlatform platform(root, keys, &console);
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        goToTab(person, app, 1);
        person.waitUntilIdle();
        person.settle();
        report("a file is there to install", app.snapshot().rows.size() == 1, format("%d", (int)app.snapshot().rows.size()));
        person.press(ButtonPlus);
        person.shot("real_menu_files");
        person.press(ButtonB);
        chooseInMenu(person, app, "Install the ticked");
        report("installing asks first", app.snapshot().dialog == "Install this game?", app.snapshot().dialog);
        person.shot("real_menu_install_confirm");
        person.press(ButtonA);
        person.waitUntilIdle();
        report("installing from the menu finishes", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        person.press(ButtonA);
        std::string problem = console.checkConsistent(keyset);
        report("it's installed, and whole", problem.empty() && console.listApplications().size() == 1, problem);
        report("whatever the options say", readText(root + "/data/settings.txt").find("installgamefiles=1") == std::string::npos);

        // Saved compressed, the NSP takes the original's place on the list, as it's the same game and as new. Installing it takes no compressing.
        goToOption(person, app, "NSP files go in");
        person.press(ButtonA);
        while (!app.snapshot().browsing.empty())
            person.press(ButtonB);
        person.press(ButtonA);
        person.press(ButtonA);
        person.press(ButtonX);
        goToTab(person, app, 1);
        person.waitUntilIdle();
        person.settle();
        person.press(ButtonY);
        if (!app.snapshot().rows.empty() && !app.snapshot().rows[0].ticked)
            person.press(ButtonY);
        person.press(ButtonX);
        person.press(ButtonA);
        person.waitUntilIdle();
        report("saving it finishes", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        person.press(ButtonA);
        person.waitUntilIdle();
        person.settle();
        App::Snapshot both = app.snapshot();
        report("what was made of it takes the original's place on the list", both.rows.size() == 1 && both.rows[0].compressed && both.rows[0].notes == 1,
               both.rows.empty() ? std::string("none") : format("%d rows, compressed %d, %d notes", (int)both.rows.size(), both.rows[0].compressed ? 1 : 0, both.rows[0].notes));
        person.shot("real_files_with_output");

        std::string saved;
        for (auto& entry : sys::list(games)) {
            if (entry.name.find("[LZ4]") != std::string::npos)
                saved = games + "/" + entry.name;
        }
        report("it's where it was asked to go", !saved.empty());
        sys::removeTree(root + "/console4");
        SimConsole again(root + "/console4", keyset);
        (void)again;
    }
    {
        std::string games = root + "/sd/games";
        sys::removeTree(root + "/console5");
        sys::removeFile(root + "/data/list.cache");
        std::string original;
        for (auto& entry : sys::list(games)) {
            if (entry.name.find("[LZ4]") == std::string::npos)
                original = games + "/" + entry.name;
        }
        Bytes keep = readWhole(original);
        sys::removeFile(original);
        SimConsole console(root + "/console5", keyset);
        HostPlatform platform(root, keys, &console);
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        goToTab(person, app, 1);
        person.waitUntilIdle();
        person.settle();
        report("only what was made of it is left", app.snapshot().rows.size() == 1, format("%d", (int)app.snapshot().rows.size()));

        // With compression off, repacking it would only uncompress it: it says how big that comes out, starts unticked, and ticked by
        // hand, it isn't asked about as coming out the same size.
        goToOption(person, app, "Compression");
        person.press(ButtonRight);
        goToTab(person, app, 1);
        person.waitUntilIdle();
        person.settle();
        App::Snapshot off = app.snapshot();
        bool grows = off.rows.size() == 1 && off.rows[0].hasEstimate && off.rows[0].after > off.rows[0].now;
        report("with compression off, a compressed file says it would grow", grows && !off.rows[0].ticked && off.rows[0].note.find("grow") != std::string::npos,
               off.rows.empty() ? std::string("none") : format("%lld now, %lld after, ticked %d: %s", (long long)off.rows[0].now, (long long)off.rows[0].after, off.rows[0].ticked ? 1 : 0, off.rows[0].note.c_str()));
        person.press(ButtonA);
        person.press(ButtonX);
        report("and ticked by hand, it isn't called the same size", app.snapshot().dialog.find("no update") == std::string::npos && !app.snapshot().dialog.empty(), app.snapshot().dialog);
        person.press(ButtonB);
        goToOption(person, app, "Compression");
        person.press(ButtonRight);
        goToTab(person, app, 1);
        person.waitUntilIdle();
        person.settle();

        person.press(ButtonY);
        if (!app.snapshot().rows.empty() && !app.snapshot().rows[0].ticked)
            person.press(ButtonY);
        chooseInMenu(person, app, "Install the ticked");
        person.press(ButtonA);
        person.waitUntilIdle();
        report("installing what's already compressed finishes", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        person.press(ButtonA);
        std::string log = readText(root + "/data/log.txt");
        report("without compressing it all over again", log.find("Game data of") == std::string::npos || log.find("Game data of") < log.find("Compressing the game data"), log.substr(0, 200));
        std::string problem = console.checkConsistent(keyset);
        report("and it's whole", problem.empty(), problem);
        sys::File out(original, sys::File::Mode::Create);
        out.write(0, keep.data(), keep.size());
    }

    // An extracted game gets installed the same way.
    {
        sys::removeTree(root + "/console6");
        wipeData(root);
        sys::removeTree(root + "/sd/games");
        sys::removeTree(root + "/output");
        {
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(nsps.size() > 2 ? nsps[2] : nsps[0]));
            set.group();
            extractTitle(set.jobs.at(0), set, root + "/output/extracted");
        }
        SimConsole console(root + "/console6", keyset);
        HostPlatform platform(root, keys, &console);
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        goToTab(person, app, 1);
        person.waitUntilIdle();
        person.settle();
        report("the extracted game is there to install", app.snapshot().rows.size() == 1, format("%d", (int)app.snapshot().rows.size()));
        chooseInMenu(person, app, "Install the ticked");
        report("installing it asks first", app.snapshot().dialog == "Install this game?", app.snapshot().dialog);
        person.press(ButtonA);
        person.waitUntilIdle();
        report("installing an extracted game finishes", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        person.press(ButtonA);
        std::string problem = console.checkConsistent(keyset);
        report("it's on the console, and whole", problem.empty() && console.listApplications().size() == 1, problem);
        auto stray = console.strayFiles();
        report("and nothing's left lying around", stray.empty(), stray.empty() ? "" : stray[0]);
        report("and the folder's still there", !sys::list(root + "/output/extracted").empty());
    }

    // However much memory there is to spare, none of the new copy is kept in it: it's all read back from the card, and checked as it's read.
    for (const char* memory : {"2147483648"}) {
        _putenv_s("HBP_TEST_SPARE_MEMORY", memory);
        auto console = freshConsole();
        i64 before = 0;
        for (u64 id : console->listApplications())
            before += console->describe(id).totalBytes();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        openSettings(person, app);
        report("there's no setting for checking", optionValue(app, "Check what's written") == "(no such option)", optionValue(app, "Check what's written"));
        goToTab(person, app, 0);
        person.settle();
        person.press(ButtonX);
        report("it says it checks the copy", app.snapshot().dialogText.find("written and checked") != std::string::npos, app.snapshot().dialogText);
        person.press(ButtonA);
        std::string seen;
        std::map<std::string, bool> labels;
        double start = sys::now();
        while (app.busy() && sys::now() - start < 3600) {
            person.frame();
            std::string label = app.snapshot().label;
            if (!label.empty() && !labels[label]) {
                labels[label] = true;
                seen += label + "; ";
                if (label.find("Working out") == 0)
                    person.shot("real_job_hash");
            }
            sys::sleepMs(2);
        }
        person.settle();
        printf("  with %s bytes to spare: %s\n", memory, seen.c_str());
        report("it finishes", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        report("all of it read back and checked", seen.find("Checking every byte of the new copy") != std::string::npos && seen.find("Hashing what's still in memory") == std::string::npos, seen);
        person.press(ButtonA);
        healthy(*console, "with memory to spare");
        i64 after = 0;
        for (u64 id : console->listApplications())
            after += console->describe(id).totalBytes();
        report("and it comes out smaller all the same", after < before);
        startOver(root);
    }
    _putenv_s("HBP_TEST_SPARE_MEMORY", "");

    // What's saved is still there the next time, and is said if it can't be.
    {
        auto console = freshConsole();
        std::string folderOnCard = root + "/sd/somewhere";
        sys::makeDirectories(folderOnCard);
        std::map<std::string, std::string> wanted;
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            report("updating by itself is on to begin with", optionValue(app, "Keep this app up to date") == "On", optionValue(app, "Keep this app up to date"));
            goToOption(person, app, "Minimum saving");
            person.press(ButtonRight);
            person.press(ButtonRight);
            goToOption(person, app, "Hide games under 1 MB");
            person.press(ButtonRight);
            goToOption(person, app, "Compression level");
            person.press(ButtonLeft);
            goToOption(person, app, "Installed games");
            person.press(ButtonRight);
            goToOption(person, app, "Game files and folders");
            person.press(ButtonRight);
            goToOption(person, app, "Once a file is installed");
            person.press(ButtonRight);
            goToOption(person, app, "Run the processor faster");
            person.press(ButtonRight);
            goToOption(person, app, "Keep this app up to date");
            person.press(ButtonRight);
            goToOption(person, app, "NSP files go in");
            person.press(ButtonA);
            while (!app.snapshot().browsing.empty())
                person.press(ButtonB);
            person.press(ButtonA);
            person.press(ButtonA);
            person.press(ButtonX);
            goToTab(person, app, 0);
            person.press(ButtonPlus);
            person.press(ButtonRight);
            person.press(ButtonB);
            openSettings(person, app);
            for (auto& option : app.snapshot().options)
                wanted[option] = option;
            wanted.erase("Check for a new version: ");
        }
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            openSettings(person, app);
            App::Snapshot snapshot = app.snapshot();
            int same = 0;
            std::string different;
            for (auto& option : snapshot.options) {
                if (wanted.count(option))
                    same++;
                else if (option.rfind("Check for a new version", 0) != 0)
                    different += option + "; ";
            }
            report("every setting is as it was left", different.empty(), different);
            report("and they aren't the usual ones", optionValue(app, "Minimum saving") != "Off" && optionValue(app, "Run the processor faster") == "Off" && optionValue(app, "Keep this app up to date") == "Off", optionValue(app, "Minimum saving"));
            report("the order of the list is kept too", app.snapshot().sortedBy == "largest first", app.snapshot().sortedBy);
        }

        // Everything's kept twice, as there's no knowing what could happen to one of them.
        std::string settings = root + "/data/settings.txt";
        std::string answers = root + "/data/analysis.txt";
        report("the settings are kept in a second place", readText(root + "/config/settings.txt") == readText(settings));
        report("and so is what's been worked out", readText(root + "/config/analysis.txt") == readText(answers));
        report("what's kept ends properly", readText(settings).size() > 4 && readText(settings).substr(readText(settings).size() - 4) == "end\n");
        sys::removeFile(settings);
        sys::removeFile(answers);
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            report("the second place is enough for the settings", optionValue(app, "Run the processor faster") == "Off", optionValue(app, "Run the processor faster"));
            std::string log = readText(root + "/data/log.txt");
            report("and what's been worked out", log.find("Remembered from last time: 0 answers") == std::string::npos, log.substr(0, 300));
            report("and the log says where they came from", log.find("settings.txt read from " + root + "/config") != std::string::npos, log.substr(0, 500));
            report("and what was in the folders", log.find("In " + root + "/config: ") != std::string::npos, log.substr(0, 500));
        }

        // Half a file: the power went while it was being written.
        {
            std::string whole = readText(root + "/config/settings.txt");
            writeText(settings, whole.substr(0, whole.size() / 2));
            writeText(root + "/config/settings.txt", whole);
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            report("half a file gets passed over for the whole one", optionValue(app, "Run the processor faster") == "Off" && optionValue(app, "Keep this app up to date") == "Off", optionValue(app, "Keep this app up to date"));
        }
        report("there's only the one log", !sys::exists(root + "/data/log.1.txt") && !sys::exists(root + "/data/log.2.txt") && sys::exists(root + "/data/log.txt"));

        // Left waiting beside where it goes, by a power cut at just the wrong moment.
        sys::removeFile(root + "/config/settings.txt");
        sys::renameFile(settings, settings + ".new");
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            report("settings that didn't get their name are still found", optionValue(app, "Run the processor faster") == "Off", optionValue(app, "Run the processor faster"));
        }
        sys::removeFile(settings);
        sys::removeFile(settings + ".new");
        sys::removeFile(root + "/config/settings.txt");

        // Something in the way of the file itself, in both places.
        sys::makeDirectories(settings + "/in the way");
        sys::makeDirectories(root + "/config/settings.txt/in the way");
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            // The settings can't be kept from the start, and that's said as soon as the list is up, before anything is pressed.
            person.settle();
            report("not being able to save gets said", app.snapshot().dialog == "That couldn't be saved", app.snapshot().dialog);
            printf("  %s\n", app.snapshot().dialogText.c_str());
            person.shot("real_cant_save");
        }
        sys::removeTree(settings);
        sys::removeTree(root + "/config/settings.txt");
        sys::removeFile(settings + ".new");
    }

    // Asking to compress while another list is still coming up: what's asked for goes first, and the list picks up again afterwards.
    {
        auto console = freshConsole();
        sys::removeTree(root + "/sd/games");
        sys::makeDirectories(root + "/sd/slow/1/2/3/4/5/6");
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            _putenv_s("HBP_TEST_SLOW_LIST_MS", "2000");
            goToTab(person, app, 1);
            person.waitFor([&] { return app.snapshot().scanning; }, 10);
            goToTab(person, app, 0);
            bool anyTicked = false;
            for (auto& row : app.snapshot().rows)
                anyTicked = anyTicked || row.ticked;
            report("with games ticked and game files still coming up", anyTicked && app.snapshot().scanning);
            double started = sys::now();
            person.press(ButtonX);
            person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 60);
            double waited = sys::now() - started;
            report("compressing asks straight away", app.snapshot().dialog.find("Compress") == 0 && waited < 5, format("%s after %.1f s", app.snapshot().dialog.c_str(), waited));
            person.press(ButtonB);
            _putenv_s("HBP_TEST_SLOW_LIST_MS", "");
            person.waitUntilIdle();
            goToTab(person, app, 1);
            person.waitUntilIdle();
            report("and game files come up afterwards", app.snapshot().dialog.empty() && !app.snapshot().scanning, app.snapshot().dialog);
        }
        sys::removeTree(root + "/sd/slow");
    }

    // X while it's still analyzing, backing out of it, and X again: the question comes up from one press each time, and analysis isn't left stopped.
    {
        auto console = freshConsole();
        sys::makeDirectories(root + "/sd/slow/1/2/3/4/5/6");
        _putenv_s("HBP_TEST_SLOW_LIST_MS", "2000");
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            bool analyzing = person.waitFor([&] { return app.snapshot().analyzing; }, 20);
            person.press(ButtonX);
            person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 30);
            report("X while analyzing asks to compress", analyzing && app.snapshot().dialog.find("Compress") == 0, app.snapshot().dialog);
            person.press(ButtonB);
            person.settle();
            // Backed out of, what was going on goes on by itself, as if X hadn't been pressed.
            bool goesOn = person.waitFor([&] { return app.snapshot().analyzing; }, 10);
            report("backing out lets analysis go on by itself", goesOn, app.snapshot().screen);
            if (!goesOn && goToMenuRow(person, app, "Go on analyzing")) {
                person.press(ButtonA);
                person.waitFor([&] { return app.snapshot().analyzing; }, 10);
            }
            bool stillAnalyzing = app.snapshot().analyzing;
            person.press(ButtonX);
            person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 30);
            report("X again asks from one press", app.snapshot().dialog.find("Compress") == 0, format("%s, %s", app.snapshot().dialog.c_str(), stillAnalyzing ? "while analyzing" : "after analyzing"));
            person.shot("real_x_again");
            person.press(ButtonB);
        }
        _putenv_s("HBP_TEST_SLOW_LIST_MS", "");
        sys::removeTree(root + "/sd/slow");
    }

    waitingOnPc();

    // The same on the list of game files itself, while it comes up again with what was there last time.
    {
        const std::string& alone = nsps.size() > 2 ? nsps[2] : nsps[0];
        sys::makeDirectories(root + "/sd/games");
        {
            Bytes bytes = readWhole(alone);
            sys::File out(root + "/sd/games/alone.nsp", sys::File::Mode::Create);
            out.write(0, bytes.data(), bytes.size());
        }
        sys::makeDirectories(root + "/sd/slow/1/2/3/4/5/6");
        auto console = freshConsole();
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToTab(person, app, 1);
            person.waitUntilIdle();
        }
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            _putenv_s("HBP_TEST_SLOW_LIST_MS", "2000");
            goToTab(person, app, 1);
            person.waitFor([&] { return app.snapshot().scanning && !app.snapshot().rows.empty(); }, 10);
            bool anyTicked = false;
            for (auto& row : app.snapshot().rows)
                anyTicked = anyTicked || row.ticked;
            report("with a game file ticked and its list still coming up", anyTicked && app.snapshot().scanning, format("%d rows", (int)app.snapshot().rows.size()));
            double started = sys::now();
            person.press(ButtonX);
            person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 60);
            double waited = sys::now() - started;
            std::string dialog = app.snapshot().dialog;
            report("pressing X on it asks straight away", !dialog.empty() && dialog.find("Nothing") == std::string::npos && waited < 5, format("%s after %.1f s", dialog.c_str(), waited));
            person.press(ButtonB);
            _putenv_s("HBP_TEST_SLOW_LIST_MS", "");
            person.waitUntilIdle();
            report("and the list comes up afterwards", app.snapshot().dialog.empty() && !app.snapshot().scanning && !app.snapshot().rows.empty(), app.snapshot().dialog);
        }
        sys::removeTree(root + "/sd/slow");
        sys::removeTree(root + "/sd/games");
    }

    // The buttons: B to quit, L and R all the way round, and sorting in the menu.
    {
        auto console = freshConsole();
        sys::removeTree(root + "/sd/games");
        sys::makeDirectories(root + "/sd/games");
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            // While a list is still being looked through, B stops that, so it waits for the lists, with analyzing still going.
            person.waitFor([&] { return app.snapshot().analyzing && !app.snapshot().scanning; }, 60);
            person.press(ButtonB);
            report("B while it's analyzing asks", app.snapshot().dialog == "Quit hacBrewPackLZ?" && app.snapshot().dialogText.find("analyzing") != std::string::npos, app.snapshot().dialog);
            person.shot("real_quit_question");
            person.press(ButtonB);
            report("and B again says no", !app.wantsToQuit() && app.snapshot().dialog.empty(), app.snapshot().dialog);
            person.waitUntilIdle();
            report("what it was doing got done", !app.snapshot().rows.empty() && app.snapshot().rows[0].hasEstimate);

            // Analyzing again.
            std::string log = readText(root + "/data/log.txt");
            size_t before = 0;
            for (size_t at = log.find("Analyzing "); at != std::string::npos; at = log.find("Analyzing ", at + 1))
                before++;
            chooseInMenu(person, app, "Analyze the ticked games again");
            report("analyzing again forgets the answers", person.waitFor([&] { return app.snapshot().analyzing; }, 20));
            person.waitUntilIdle();
            log = readText(root + "/data/log.txt");
            size_t after = 0;
            for (size_t at = log.find("Analyzing "); at != std::string::npos; at = log.find("Analyzing ", at + 1))
                after++;
            report("and works them out again", after == before * 2, format("%d then %d", (int)before, (int)after));

            // Sorting.
            person.press(ButtonStickR);
            report("the right stick does nothing now", app.snapshot().sortedBy == "best saving first", app.snapshot().sortedBy);
            person.press(ButtonPlus);
            report("+ opens the menu", !app.snapshot().menu.empty());
            person.shot("real_menu");

            // Holding a direction down keeps going.
            {
                InputState held;
                held.held = ButtonDown;
                for (int i = 0; i < 60; i++)
                    person.frame(held);
                person.frame();
                report("holding down goes down the menu", app.snapshot().menuFocus >= 2, format("%d", app.snapshot().menuFocus));
                held.held = ButtonUp;
                for (int i = 0; i < 90; i++)
                    person.frame(held);
                person.frame();
                report("and holding up comes back", app.snapshot().menuFocus == 0, format("%d", app.snapshot().menuFocus));
            }
            report("with the sorting on one row", app.snapshot().menu[0] == "Sort by: Best saving", app.snapshot().menu[0]);
            person.press(ButtonRight);
            report("changed with right", app.snapshot().menu[0] == "Sort by: Size" && app.snapshot().sortedBy == "largest first", app.snapshot().menu[0]);
            person.press(ButtonRight);
            report("and right again", app.snapshot().menu[0] == "Sort by: Name" && app.snapshot().sortedBy == "by name", app.snapshot().menu[0]);
            person.press(ButtonRight);
            report("and round to the start", app.snapshot().menu[0] == "Sort by: Best saving", app.snapshot().menu[0]);
            person.press(ButtonLeft);
            report("and back with left", app.snapshot().menu[0] == "Sort by: Name", app.snapshot().menu[0]);
            person.press(ButtonRight);
            person.press(ButtonB);
            report("B closes the menu without quitting", app.snapshot().menu.empty() && !app.wantsToQuit());

            // Refreshing.
            {
                const std::string& alone = nsps.size() > 2 ? nsps[2] : nsps[0];
                // Extracted games left by what came before are on this list too, so they go first.
                sys::removeTree(root + "/output/extracted");
                goToTab(person, app, 1);
                person.waitUntilIdle();
                chooseInMenu(person, app, "Refresh the list");
                person.waitUntilIdle();
                person.settle();
                person.settle();
                report("nothing's there to begin with", app.snapshot().rows.empty(), format("%d", (int)app.snapshot().rows.size()));
                Bytes bytes = readWhole(alone);
                sys::File out(root + "/sd/games/new one.nsp", sys::File::Mode::Create);
                out.write(0, bytes.data(), bytes.size());
                out.close();
                chooseInMenu(person, app, "Refresh the list");
                person.waitUntilIdle();
                person.settle();
                report("refreshing finds what's new", app.snapshot().rows.size() == 1, format("%d", (int)app.snapshot().rows.size()));
            }

            // L and R only go between the overview and the lists: the settings are only come to on purpose, from the + menu.
            goToTab(person, app, 0);
            person.press(ButtonL);
            report("L from the first list goes to the overview, not the settings", app.snapshot().overview && app.snapshot().screen == "Main", app.snapshot().screen);
            person.press(ButtonR);
            report("and R back round", app.snapshot().tab == 0, format("%d", app.snapshot().tab));
            report("the + menu has the settings", chooseInMenu(person, app, "Settings ...") && app.snapshot().screen == "Options", app.snapshot().screen);
            person.shot("real_settings_screen");
            person.press(ButtonL);
            person.press(ButtonR);
            report("L and R do nothing in the settings", app.snapshot().screen == "Options" && app.snapshot().tab == 0, app.snapshot().screen);
            person.press(ButtonB);
            report("B on the settings goes back to the list", app.snapshot().screen == "Main" && app.snapshot().dialog.empty() && app.snapshot().tab == 0, app.snapshot().screen + " " + app.snapshot().dialog);
            person.press(ButtonB);
            report("and B there asks to quit", !app.wantsToQuit() && app.snapshot().dialog == "Quit hacBrewPackLZ?", app.snapshot().dialog);
            person.shot("real_quit_settings");
            person.press(ButtonA);
            report("and A goes", app.wantsToQuit());
        }
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            // Something's on the screen before what was there last time is read, which is what takes a while on a full card.
            bool drawnEarly = false;
            size_t rowsThen = 0;
            app.whenItCanBeDrawn = [&] {
                drawnEarly = true;
                rowsThen = app.snapshot().rows.size();
                std::vector<u32> pixels(1280 * 720);
                Canvas canvas(pixels.data(), 1280, 720, 1280);
                app.drawOpening(canvas);
                writePng(shots + "/real_opening.png", pixels.data(), 1280, 720);
            };
            app.start();
            report("it shows it's opening before the lists are read", drawnEarly && rowsThen == 0 && !app.snapshot().rows.empty(), format("%d, %d then, %d after", drawnEarly ? 1 : 0, (int)rowsThen, (int)app.snapshot().rows.size()));
            Person person(app, shots);
            person.waitUntilIdle();
            std::string log = readText(root + "/data/log.txt");
            report("the answers are still there next time", log.find("Analyzing") == std::string::npos, log.substr(0, 300));
            report("as the log says", log.find("Remembered from last time") != std::string::npos);
            person.press(ButtonB);
            report("B asks even with nothing going on", !app.wantsToQuit() && app.snapshot().dialog == "Quit hacBrewPackLZ?", app.snapshot().dialog);
            person.press(ButtonB);
            report("and B again stays", !app.wantsToQuit() && app.snapshot().dialog.empty(), app.snapshot().dialog);
        }

        // A game that couldn't be analyzed doesn't get tried again every time, only when it's asked for.
        {
            std::string answers = readText(root + "/data/analysis.txt");
            size_t tab = answers.find('\t');
            size_t end = answers.find('\n', tab);
            writeText(root + "/data/analysis.txt", answers.substr(0, tab + 1) + "failed It didn't work for some reason" + answers.substr(end));
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            std::string log = readText(root + "/data/log.txt");
            report("what couldn't be worked out isn't tried again by itself", log.find("Analyzing") == std::string::npos);
            bool said = false;
            for (auto& row : app.snapshot().rows)
                said = said || row.note == "It didn't work for some reason";
            report("and it says why", said);
            std::string line;
            for (auto& row : app.snapshot().all)
                line = row.state == "Failed" ? row.second : line;
            report("on the list, plainly, and where to read the rest", line == "Something went wrong. Its details say what.", line);
            person.shot("real_remembered_failure");
            App::Snapshot failedOne = app.snapshot();
            size_t which = 0;
            while (which < failedOne.rows.size() && failedOne.rows[which].note != "It didn't work for some reason")
                which++;
            report("and it comes up unticked", which < failedOne.rows.size() && !failedOne.rows[which].ticked);
            for (size_t i = 0; i < which; i++)
                person.press(ButtonDown);
            // The list says it in a few plain words, so the failure itself is in its details.
            bool about = chooseInMenu(person, app, "Details") && person.waitFor([&] { return app.snapshot().insideReady; }, 60);
            std::string noted;
            for (auto& note : app.snapshot().detailsNotes)
                noted += note + "; ";
            report("and its details say it in full", about && noted.find("It didn't work for some reason") != std::string::npos, noted);
            person.shot("real_about_failure");
            person.press(ButtonB);
            report("which close again", app.snapshot().screen == "Main" && app.snapshot().dialog.empty(), app.snapshot().screen + " " + app.snapshot().dialog);
            person.press(ButtonA);
            chooseInMenu(person, app, "Analyze the ticked games again");
            person.waitUntilIdle();
            bool allKnown = true;
            for (auto& row : app.snapshot().rows)
                allKnown = allKnown && row.hasEstimate && row.note != "It didn't work for some reason";
            report("until it's asked to", allKnown);
        }
        // One that wanted keys gets another go each time, as they may have been dumped again since.
        {
            std::string answers = readText(root + "/data/analysis.txt");
            size_t tab = answers.find('\t');
            size_t end = answers.find('\n', tab);
            writeText(root + "/data/analysis.txt", answers.substr(0, tab + 1) + "failed The key area key for generation 0b is not in your keys, it is needed for x.nca" + answers.substr(end));
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            bool allKnown = true;
            for (auto& row : app.snapshot().rows)
                allKnown = allKnown && row.hasEstimate && row.note.find("not in your keys") == std::string::npos;
            report("a game that wanted keys is tried again by itself", allKnown);
            report("and that isn't kept", readText(root + "/data/analysis.txt").find("not in your keys") == std::string::npos);
        }
        startOver(root);
        sys::removeFile(root + "/data/list.cache");
        sys::removeTree(root + "/sd/games");
    }

    // Where apps keep their own things is only looked through near the top, and hidden folders not at all, as that's where thousands of
    // folders with no games in them are.
    {
        const std::string& alone = nsps.size() > 2 ? nsps[2] : nsps[0];
        Bytes bytes = readWhole(alone);
        auto put = [&](const std::string& folder) {
            sys::makeDirectories(folder);
            sys::File out(folder + "/alone.nsp", sys::File::Mode::Create);
            out.write(0, bytes.data(), bytes.size());
        };
        auto console = freshConsole();
        sys::removeTree(root + "/sd/games");
        put(root + "/sd/switch/someapp/games");
        put(root + "/sd/switch/someapp/cache/deep");
        put(root + "/sd/.hidden");
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            goToTab(person, app, 1);
            person.waitUntilIdle();
            std::string log = readText(root + "/data/log.txt");
            size_t at = log.find("Went through");
            std::string said = at == std::string::npos ? "" : log.substr(at, log.find('\n', at) - at);
            report("a game file just inside an app's folder is found, and not those deeper down or in hidden folders", said.find("found 1 game files") != std::string::npos, said);
            report("and what's left out is said", said.find("left out 2 ") != std::string::npos, said);
        }
        sys::removeTree(root + "/sd/switch");
        sys::removeTree(root + "/sd/.hidden");
    }

    // The list on the screen comes first.
    {
        sys::makeDirectories(root + "/sd/many");
        for (int i = 0; i < 1500; i++)
            writeText(root + "/sd/many/" + format("nothing %d.nsp", i), "not a game");
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        person.press(ButtonR);
        person.waitFor([&] { return app.snapshot().scanning; }, 5);
        person.press(ButtonL);
        report("going back to a list goes back to it", app.snapshot().tab == 0);
        person.waitUntilIdle();
        std::string log = readText(root + "/data/log.txt");
        report("and the other one carries on being looked through", log.find("Stopped") == std::string::npos && log.find("of them opened") != std::string::npos, log.substr(log.size() > 300 ? log.size() - 300 : 0));
        sys::removeTree(root + "/sd/many");
        sys::removeFile(root + "/data/list.cache");
    }

    // A list that's taking a while, as a big drive would, stops with B, rather than the app, and stays stopped until it's refreshed.
    {
        sys::makeDirectories(root + "/sd/many");
        for (int i = 0; i < 1500; i++)
            writeText(root + "/sd/many/" + format("nothing %d.nsp", i), "not a game");
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        person.press(ButtonR);
        person.waitFor([&] { return app.snapshot().scanning; }, 5);
        person.shot("real_files_looking");
        person.press(ButtonB);
        report("B while looking doesn't quit", !app.wantsToQuit());
        person.waitFor([&] { return !app.snapshot().scanning && app.snapshot().toast.find("Stopped looking") == 0; }, 10);
        report("it stops looking, and says so", !app.snapshot().scanning && app.snapshot().toast.find("Stopped looking") == 0, app.snapshot().toast);
        person.shot("real_files_stopped");
        person.waitUntilIdle();
        std::string log = readText(root + "/data/log.txt");
        report("and doesn't start again by itself", log.find("Stopped looking for game files") != std::string::npos && log.find("of them opened") == std::string::npos,
               log.substr(log.size() > 300 ? log.size() - 300 : 0));
        report("after which B quits as before", (person.press(ButtonB), app.wantsToQuit() || !app.snapshot().dialog.empty()));
        sys::removeTree(root + "/sd/many");
        sys::removeFile(root + "/data/list.cache");
    }

    // Games that small with nothing else to them are hidden, unless that's turned off.
    {
        auto console = freshConsole();
        sys::removeTree(root + "/output");
        {
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(nsps.size() > 2 ? nsps[2] : nsps[0]));
            set.group();
            extractTitle(set.jobs.at(0), set, root + "/output/extracted");
        }
        std::string game;
        for (auto& entry : sys::list(root + "/output/extracted"))
            game = root + "/output/extracted/" + entry.name;
        sys::removeTree(game + "/romfs");
        i64 bytes = ExtractedGame::open(game).inputBytes;
        if (bytes < (1 << 20)) {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToTab(person, app, 1);
            person.waitUntilIdle();
            report("a game under 1 MB stays out of the list", app.snapshot().rows.empty(), format("%d", (int)app.snapshot().rows.size()));
            goToOption(person, app, "Hide games under 1 MB");
            person.press(ButtonRight);
            goToTab(person, app, 1);
            person.waitUntilIdle();
            person.settle();
            report("until that's turned off", app.snapshot().rows.size() == 1, format("%d", (int)app.snapshot().rows.size()));
        } else {
            printf("  that game is too large to try this with: %lld bytes\n", (long long)bytes);
        }
        sys::removeTree(root + "/output");
        startOver(root);
        sys::removeFile(root + "/data/list.cache");
    }

    // Updating itself.
    if (const char* folder = getenv("UPDATE_DIR")) {
        std::string dir = folder;
        const std::string listing = "https://gitlab.com/api/v4/projects/ismaellozanoperezserrano%2Fhacbrewpacklz/releases?per_page=1";
        const std::string base = "https://gitlab.com/ismaellozanoperezserrano/hacbrewpacklz/-/releases/v9.9.9/downloads/";
        std::string program = root + "/program.bin";
        auto attempt = [&](const std::function<void(HostPlatform&)>& setUp) {
            writeText(program, "the old program");
            auto console = freshConsole();
            HostPlatform platform(root, keys, console.get());
            platform.online = true;
            platform.program = program;
            sys::makeDirectories(root + "/data");
            writeText(root + "/data/settings.txt", "updates=0\nmoresettings=1\nwritelog=1\nmeasureall=0\ndeleteasked=1\n");
            std::string text = "[{\"name\":\"hacBrewPackLZ 9.9.9\",\"tag_name\":\"v9.9.9\",\"description\":\"\"}]";
            platform.web[listing] = Bytes(text.begin(), text.end());
            platform.web[base + "hacbrewpacklz-update.txt"] = readWhole(dir + "/hacbrewpacklz-update.txt");
            platform.web[base + "hacbrewpacklz-update.sig"] = readWhole(dir + "/hacbrewpacklz-update.sig");
            platform.web[base + "program.bin"] = readWhole(dir + "/program.bin");
            setUp(platform);
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToOption(person, app, "Check for a new version");
            person.press(ButtonA);
            person.waitFor([&] { return !app.snapshot().dialog.empty() || app.snapshot().updateState.find("latest") != std::string::npos; }, 10);
            App::Snapshot result = app.snapshot();
            person.shot("real_update_" + std::to_string(sys::list(shots).size()));
            return result;
        };

        Bytes wanted = readWhole(dir + "/program.bin");
        App::Snapshot done = attempt([](HostPlatform&) {});
        report("a signed release gets installed", readWhole(program) == wanted, done.dialog + " " + done.dialogText);
        report("and says so", done.dialog.find("Updated to version 9.9.9") == 0, done.dialog);

        done = attempt([&](HostPlatform& platform) { platform.web[base + "hacbrewpacklz-update.sig"][20] ^= 1; });
        report("a bad signature keeps the old program", readText(program) == "the old program" && done.dialogText.find("signature") != std::string::npos, done.dialogText);

        done = attempt([&](HostPlatform& platform) { platform.web[base + "hacbrewpacklz-update.txt"][10] ^= 1; });
        report("so does a changed list of what's in the release", readText(program) == "the old program" && done.dialogText.find("signature") != std::string::npos, done.dialogText);

        done = attempt([&](HostPlatform& platform) { platform.web[base + "program.bin"][4] ^= 1; });
        report("so does a damaged download", readText(program) == "the old program" && done.dialogText.find("isn't what the release says") != std::string::npos, done.dialogText);

        done = attempt([&](HostPlatform& platform) {
            std::string older = "[{\"tag_name\":\"v1.0.0\"}]";
            platform.web[listing] = Bytes(older.begin(), older.end());
        });
        report("an older release gets left alone", readText(program) == "the old program" && done.updateState.find("latest") != std::string::npos, done.updateState);

        done = attempt([&](HostPlatform& platform) { platform.online = false; });
        report("not being online gets said", readText(program) == "the old program" && done.dialog == "Not connected", done.dialog);
        startOver(root);

        // Left as it is to begin with, it goes and looks by itself.
        {
            writeText(program, "the old program");
            auto console = freshConsole();
            HostPlatform platform(root, keys, console.get());
            platform.online = true;
            platform.program = program;
            std::string text = "[{\"tag_name\":\"v9.9.9\"}]";
            platform.web[listing] = Bytes(text.begin(), text.end());
            platform.web[base + "hacbrewpacklz-update.txt"] = readWhole(dir + "/hacbrewpacklz-update.txt");
            platform.web[base + "hacbrewpacklz-update.sig"] = readWhole(dir + "/hacbrewpacklz-update.sig");
            platform.web[base + "program.bin"] = readWhole(dir + "/program.bin");
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 30);
            report("it updates itself without being asked", readWhole(program) == wanted && app.snapshot().dialog.find("Updated to version 9.9.9") == 0, app.snapshot().dialog);
        }
        startOver(root);
    }

    // Too old a firmware.
    {
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        platform.newFirmware = false;
        platform.battery = 12;
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        person.press(ButtonX);
        report("too old a firmware gets told so", app.snapshot().dialog.find("too old") != std::string::npos, app.snapshot().dialog);
        person.shot("real_old_firmware");
        person.press(ButtonA);

    }

    // What's been done goes out of the way, and what's next is in front of whoever comes back.
    {
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        App::Snapshot before = app.snapshot();
        report("there are games to choose from", before.rows.size() >= 2, format("%d", (int)before.rows.size()));
        if (before.rows.size() >= 2) {
            std::string done = before.rows[0].name, next = before.rows[1].name;
            person.press(ButtonY);
            if (app.snapshot().tickHint == "Untick all")
                person.press(ButtonY);
            if (app.snapshot().rows[0].ticked == false)
                person.press(ButtonA);
            for (size_t i = 1; i < before.rows.size(); i++) {
                person.press(ButtonDown);
                if (app.snapshot().rows[i].ticked)
                    person.press(ButtonA);
            }
            person.press(ButtonX);
            report("only the first is going", app.snapshot().dialog == "Compress this game?", app.snapshot().dialog);
            person.press(ButtonA);
            person.waitUntilIdle();
            person.press(ButtonA);
            person.waitUntilIdle();
            App::Snapshot after = app.snapshot();
            report("the game that's been done goes to the end of the list", after.rows.back().name == done, after.rows.back().name);
            report("and the next one's at the top", after.rows[0].name == next, after.rows[0].name);
            person.shot("real_done_at_the_end");
        }
    }

    // A game whose base can't be read doesn't count as having gone, whatever its DLC can do.
    {
        auto console = freshConsole();
        for (u64 id : console->listApplications()) {
            InstalledGame game = console->describe(id);
            bool hasDlc = std::any_of(game.metas.begin(), game.metas.end(), [](const InstalledMeta& meta) { return meta.key.type == Cnmt::MetaAddOnContent; });
            if (!hasDlc)
                continue;
            for (auto& meta : game.metas) {
                if (meta.key.type != Cnmt::MetaApplication)
                    continue;
                for (auto& content : meta.contents) {
                    if (content.type == Cnmt::ContentTypeProgram)
                        sys::removeFile(console->contentPath(meta.storage, content.id));
                }
            }
        }
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        bool saysWhy = false, bogus = false;
        for (auto& row : app.snapshot().rows) {
            saysWhy = saysWhy || !row.note.empty();
            bogus = bogus || (!row.note.empty() && row.hasEstimate);
        }
        report("a game with its base missing says so", saysWhy);
        bool tickedAnyway = false;
        for (auto& row : app.snapshot().rows)
            tickedAnyway = tickedAnyway || (!row.note.empty() && row.ticked);
        report("and doesn't stay ticked", !tickedAnyway);
        report("rather than claiming it would all go", !bogus);
        person.shot("real_base_missing");
    }

    // The first time it's opened.
    {
        auto console = freshConsole();
        sys::removeTree(root + "/data");
        sys::removeTree(root + "/config");
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.readsWelcome = true;
            person.settle();
            report("the welcome screen comes first", app.snapshot().screen == "Welcome", app.snapshot().screen);
            person.shot("real_welcome");
            for (auto& item : app.snapshot().welcome)
                printf("  %s\n", item.c_str());
            report("a minimum of 5% to begin with", optionValue(app, "Minimum saving") == "5%", optionValue(app, "Minimum saving"));
            // Every copy is checked, so that's not asked.
            std::vector<std::string> welcome = app.snapshot().welcome;
            bool asksAboutChecking = std::any_of(welcome.begin(), welcome.end(), [](const std::string& item) { return item.rfind("Check what's written", 0) == 0; });
            report("checking isn't asked about", !asksAboutChecking);
            App::Snapshot first = app.snapshot();
            report("and it starts on what's installed", first.welcome.size() >= 7 && first.welcome[0] == "Installed games: Replace them", first.welcome.empty() ? "" : first.welcome[0]);
            report("game files get installed to begin with", first.welcome.size() >= 7 && first.welcome[1] == "Game files and folders: Install them", first.welcome.size() > 1 ? first.welcome[1] : "");
            report("and updates are automatic", first.welcome.size() >= 7 && first.welcome[4] == "Keep this app up to date: On", first.welcome.size() > 4 ? first.welcome[4] : "");
            person.press(ButtonRight);
            report("which can be changed", app.snapshot().welcome[0] == "Installed games: Save an NSP" && optionValue(app, "Installed games") == "Save an NSP", app.snapshot().welcome[0]);
            person.press(ButtonLeft);
            person.press(ButtonDown);
            person.press(ButtonDown);
            person.press(ButtonA);
            report("choosing a folder goes to the file explorer", app.snapshot().screen == "Browse", app.snapshot().screen);
            person.press(ButtonMinus);
            report("and back to the welcome screen", app.snapshot().screen == "Welcome", app.snapshot().screen);
            person.press(ButtonDown);
            person.press(ButtonRight);
            report("the minimum saving is a choice", app.snapshot().welcome[3] == "Minimum saving: 10%", app.snapshot().welcome[3]);
            person.shot("real_welcome_safe");
            person.press(ButtonLeft);
            for (int i = 0; i < 8; i++)
                person.press(ButtonDown);
            person.press(ButtonA);
            report("starting goes to the list", app.snapshot().screen == "Main", app.snapshot().screen);
            person.waitUntilIdle();
            report("what's on the list got looked at in the meantime", !app.snapshot().rows.empty());
            person.press(ButtonX);
            person.press(ButtonB);
            report("and it's remembered", readText(root + "/data/settings.txt").find("welcomed=1") != std::string::npos);
        }
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            report("it only comes up the once", app.snapshot().screen == "Main", app.snapshot().screen);
            Person person(app, shots);
            person.readsWelcome = true;
            person.settle();
            goToOption(person, app, "Welcome screen");
            person.press(ButtonA);
            report("and it can be brought back", app.snapshot().screen == "Welcome", app.snapshot().screen);
            person.press(ButtonPlus);
            report("and + leaves it", app.snapshot().screen == "Options", app.snapshot().screen);
        }
    }

    // A game file that's set to be saved as an NSP, installed from the + menu instead, and the folder chooser along the way.
    {
        sys::removeTree(root + "/console7");
        wipeData(root);
        sys::removeTree(root + "/sd/games");
        sys::makeDirectories(root + "/sd/games/more");
        sys::removeTree(root + "/output");
        const std::string& alone = nsps.size() > 2 ? nsps[2] : nsps[0];
        {
            Bytes bytes = readWhole(alone);
            sys::File out(root + "/sd/games/one to install.nsp", sys::File::Mode::Create);
            out.write(0, bytes.data(), bytes.size());
        }
        writeText(root + "/sd/games/not a game.txt", "nothing to see");
        SimConsole console(root + "/console7", keyset);
        HostPlatform platform(root, keys, &console);
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        report("nothing's installed to begin with", app.snapshot().rows.empty(), format("%d", (int)app.snapshot().rows.size()));
        person.press(ButtonR);
        person.waitUntilIdle();
        report("the game file is on the list, and nothing else", app.snapshot().rows.size() == 1, format("%d", (int)app.snapshot().rows.size()));

        goToMenuRow(person, app, "Only look in one folder");
        person.press(ButtonA);
        report("the + menu opens a folder chooser", app.snapshot().screen == "Browse", app.snapshot().screen);
        person.press(ButtonMinus);
        report("and - closes it", app.snapshot().screen == "Main", app.snapshot().screen);
        goToMenuRow(person, app, "Only look in one folder");
        person.press(ButtonA);
        person.press(ButtonA);
        App::Snapshot inside = app.snapshot();
        report("in folders", !inside.browseEntries.empty() && inside.browseEntries[0] == "games", inside.browseEntries.empty() ? "" : inside.browseEntries[0]);
        person.press(ButtonA);
        inside = app.snapshot();
        report("with only folders listed", inside.browseEntries.size() == 1 && inside.browseEntries[0] == "more", inside.browseEntries.empty() ? "" : inside.browseEntries.back());
        person.press(ButtonMinus);

        goToMenuRow(person, app, "Install this game");
        person.press(ButtonA);
        bool asked = person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 60);
        report("installing it asks first", asked && app.snapshot().dialog == "Install this game?", app.snapshot().dialog);
        person.shot("real_install_confirm");
        person.press(ButtonB);
        person.settle();
        report("saying no changes nothing", app.snapshot().screen == "Main" && console.listApplications().empty(), app.snapshot().screen);
        goToMenuRow(person, app, "Install this game");
        person.press(ButtonA);
        person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 60);
        person.press(ButtonA);
        person.waitUntilIdle();
        report("and it installs", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        person.shot("real_install_results");
        person.press(ButtonA);
        person.press(ButtonL);
        person.waitUntilIdle();
        person.settle();
        report("on the list of what's installed", app.snapshot().tab == 0 && app.snapshot().rows.size() == 1, format("%d", (int)app.snapshot().rows.size()));
        report("and the file is kept, as it was set to be saved", sys::exists(root + "/sd/games/one to install.nsp"));
        std::string problem = console.checkConsistent(keyset);
        report("and it's whole", problem.empty() && console.listApplications().size() == 1, problem);
        auto stray = console.strayFiles();
        report("with nothing left lying around", stray.empty(), stray.empty() ? "" : stray[0]);
        sys::removeTree(root + "/sd/games");
    }

    // Installing from a PC running hacbrewpacklz send, over the network, when there's one to try with: HBP_TEST_PC says where it is, and
    // HBP_TEST_PC_SHARE which folder it's sharing.
    pcInstall();

    printf("%d passed, %d failed\n", g_passes, g_failures);
    return g_failures > 0 ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 1 << 16);
    std::string keys;
    std::vector<std::string> plain;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-k")
            keys = argv[++i];
        else
            plain.push_back(arg);
    }
    // Nowhere to send reports, other than the stand-in a test gives it.
    _putenv_s("HBP_TEST_REPORT_SITE", "none");
    try {
        if (plain.size() == 2 && plain[0] == "icon")
            return writeIcon(plain[1]);
        if (plain.size() == 2 && plain[0] == "fonts")
            return runFonts(plain[1]);
        if (plain.size() == 2 && plain[0] == "hashtree")
            return runHashTree(atoi(plain[1].c_str()));
        if (plain.size() < 3) {
            printf("app_test -k keys folder shots demo\napp_test -k keys folder shots real base.nsp update.nsp [other.nsp]\napp_test fonts shots\napp_test icon file.jpg\n");
            return 2;
        }
        sys::makeDirectories(plain[0]);
        sys::makeDirectories(plain[1]);
        if (plain[2] == "demo")
            return runDemo(keys, plain[0], plain[1]);
        if (plain[2] == "check") {
            // An optional seed and number of steps, then the files.
            size_t next = 3;
            auto number = [&](unsigned fallback) {
                if (next < plain.size() && !plain[next].empty() && std::all_of(plain[next].begin(), plain[next].end(), ::isdigit))
                    return (unsigned)strtoul(plain[next++].c_str(), nullptr, 10);
                return fallback;
            };
            unsigned seed = number(1);
            int steps = (int)number(12);
            return runCheck(keys, plain[0], plain[1], seed, steps, std::vector<std::string>(plain.begin() + (long)next, plain.end()));
        }
        return runReal(keys, plain[0], plain[1], std::vector<std::string>(plain.begin() + 3, plain.end()));
    } catch (const std::exception& e) {
        printf("FAIL  %s\n", e.what());
        return 1;
    }
}
