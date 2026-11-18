// Replaces installed games on the pretend console, and pulls the plug at every step along the way.
//   replace_test -k keys folder base.nsp update.nsp [dlc.nsp...]
//   replace_test -k keys folder --once file.nsp [more.nsp...]   one replace, no power cuts: for a game that's too big or slow to cut at every step
//
// Whatever happens, and whenever the power goes, the game has to stay playable: every content its records list has to be there and
// be right, nothing may be left lying around after the next start, and the job has to go through when tried again.
#include <algorithm>
#include <cstdio>
#include <set>

#include "../core/console.h"
#include "sim_console.h"

using namespace hbp;

namespace {

int g_failures = 0;
int g_passes = 0;

void report(const std::string& name, bool ok, const std::string& detail = "") {
    if (ok) {
        g_passes++;
        printf("PASS  %s\n", name.c_str());
    } else {
        g_failures++;
        printf("FAIL  %s  %s\n", name.c_str(), detail.c_str());
    }
}

struct Loaded {
    std::shared_ptr<InstalledArchive> archive;
    std::unique_ptr<TitleSet> set;
    InstalledGame game;
};

Loaded loadGame(SimConsole& console, const Keyset& keyset, u64 application) {
    Loaded loaded;
    loaded.game = console.describe(application);
    loaded.archive = std::make_shared<InstalledArchive>(console, loaded.game);
    loaded.set.reset(new TitleSet(keyset));
    console.addTitleKeys(loaded.set->tickets(), keyset);
    loaded.set->add(loaded.archive);
    loaded.set->group();
    return loaded;
}

i64 replaceAll(SimConsole& console, const Keyset& keyset, const std::string& journal, u64 application, const ConvertOptions& options) {
    Replacer replacer(console, journal);
    replacer.checkConsole(Storage::Sd, options, keyset);
    i64 after = 0;
    // A title at a time, reading the console afresh each time, as the app does.
    std::set<u64> done;
    while (true) {
        Loaded loaded = loadGame(console, keyset, application);
        const TitleJob* job = nullptr;
        for (auto& candidate : loaded.set->jobs) {
            if (!done.count(candidate.main->meta.titleId)) {
                job = &candidate;
                break;
            }
        }
        if (!job)
            return after;
        done.insert(job->main->meta.titleId);
        ReplaceResult result = replacer.run(loaded.game, *job, options, keyset);
        after += result.after;
    }
}

}  // namespace

void setTestEnv(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 1 << 16);
    std::string keys, folder, highRequired;
    std::vector<std::string> inputs;
    int stride = 1;
    bool once = false;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-k")
            keys = argv[++i];
        else if (arg == "--once")
            once = true;
        else if (arg == "--stride")
            stride = atoi(argv[++i]);
        // The base game made again to ask for system version 23.0.0, as newer games do.
        else if (arg == "--high-required")
            highRequired = argv[++i];
        else if (folder.empty())
            folder = arg;
        else
            inputs.push_back(arg);
    }

    Keyset keyset;
    keyset.load(keys);
    ConvertOptions options;
    options.tempDir = folder + "/temp";
    options.threads = 2;

    std::string journal = folder + "/journal";
    auto freshConsole = [&](const std::string& name) {
        std::string path = folder + "/" + name;
        sys::removeTree(path);
        sys::removeTree(journal);
        sys::removeTree(options.tempDir);
        auto console = std::make_unique<SimConsole>(path, keyset);
        console->install(inputs, Storage::Sd);
        return console;
    };

    // One replace and the checks that come with it, for a game too big to cut the power at every step.
    if (once) {
        auto console = freshConsole("once");
        auto list = console->listApplications();
        report("the game is installed", list.size() == 1);
        std::string error;
        try {
            replaceAll(*console, keyset, journal, list.at(0), options);
        } catch (const ToolError& e) {
            error = e.what();
        }
        report("replacing it goes through", error.empty(), error);
        std::string problem = console->checkConsistent(keyset);
        report("and leaves it whole", problem.empty(), problem);
        auto stray = console->strayFiles();
        report("with nothing lying around", stray.empty(), stray.empty() ? "" : stray[0]);
        printf("%d passed, %d failed\n", g_passes, g_failures);
        return g_failures > 0 ? 1 : 0;
    }

    // The plain case, and how many steps it takes.
    u64 application;
    int steps;
    i64 before;
    int programs = 0;
    {
        auto console = freshConsole("plain");
        auto list = console->listApplications();
        report("the game is installed", list.size() == 1);
        application = list.at(0);
        before = console->describe(application).totalBytes();
        // A collection has more than one. Only the base game's count: an update has programs of its own.
        for (auto& meta : console->describe(application).metas)
            for (auto& content : meta.contents)
                programs += meta.key.type == Cnmt::MetaApplication && content.type == Cnmt::ContentTypeProgram;
        report("it starts out whole", console->checkConsistent(keyset).empty(), console->checkConsistent(keyset));

        console->losePowerAt(0);
        int startCalls = console->calls();
        i64 after = replaceAll(*console, keyset, journal, application, options);
        steps = console->calls() - startCalls;
        std::string problem = console->checkConsistent(keyset);
        report("replacing it leaves it whole", problem.empty(), problem);
        auto stray = console->strayFiles();
        report("and nothing lying around", stray.empty(), stray.empty() ? "" : stray[0]);
        report("the journal is gone afterwards", !sys::exists(journal + "/journal.txt"));
        report("the temporary folder is empty", sys::list(options.tempDir).empty());
        // The console encrypts an installed game in 16 KB blocks on the SD card, and a write that starts or ends inside one has that
        // block written again: every write starts and ends on one, but for where a content ends.
        report("every write of the new copy starts and ends on the card's 16 KB blocks", console->contentWrites > 0 && console->blockSplittingWrites == 0,
               format("%d of %d writes split a block", console->blockSplittingWrites.load(), console->contentWrites.load()));

        InstalledGame game = console->describe(application);
        bool hasPatch = false;
        for (auto& meta : game.metas)
            hasPatch = hasPatch || meta.key.type == Cnmt::MetaPatch;
        report("the update has become part of the game", !hasPatch);
        printf("  %lld bytes before, %lld after, %d console calls\n", (long long)before, (long long)game.totalBytes(), steps);
        (void)after;

        // Doing it again finds nothing left to gain but has to work the same.
        replaceAll(*console, keyset, journal, application, options);
        problem = console->checkConsistent(keyset);
        report("replacing it a second time leaves it whole", problem.empty(), problem);
        stray = console->strayFiles();
        report("and nothing lying around that time either", stray.empty(), stray.empty() ? "" : stray[0]);
    }

    // The firmware won't read it: the original has to stay, untouched.
    {
        auto console = freshConsole("refused");
        console->setFirmwareRefuses(true);
        std::string error;
        try {
            replaceAll(*console, keyset, journal, application, options);
        } catch (const ToolError& e) {
            error = e.what();
        }
        report("a firmware that refuses stops the job", error.find("sigpatches") != std::string::npos, error);
        std::string problem = console->checkConsistent(keyset);
        report("and the original is still whole", problem.empty(), problem);
        auto stray = console->strayFiles();
        report("with nothing left behind", stray.empty(), stray.empty() ? "" : stray[0]);
        report("and the same size as before", console->describe(application).totalBytes() == before);
    }

    // No room: same.
    {
        auto console = freshConsole("noroom");
        console->setFree(Storage::Sd, 1 << 20);
        std::string error;
        try {
            replaceAll(*console, keyset, journal, application, options);
        } catch (const ToolError& e) {
            error = e.what();
        }
        report("no room stops the job, and says so", error.find("enough room") != std::string::npos, error);
        std::string problem = console->checkConsistent(keyset);
        report("and the original is still whole", problem.empty(), problem);
        auto stray = console->strayFiles();
        report("with nothing left behind", stray.empty(), stray.empty() ? "" : stray[0]);
    }

    // Stopped by the person using it, part way through: same.
    {
        class StopAfter : public JobSink {
        public:
            explicit StopAfter(int reports) : _left(reports) {}
            void progress(const char*, i64, i64, i64) override {
                if (--_left <= 0)
                    stop = true;
            }

        private:
            int _left;
        };

        for (int reports : {1, 3, 8}) {
            auto console = freshConsole("stopped");
            StopAfter stopper(reports);
            setSink(&stopper);
            bool stopped = false;
            try {
                replaceAll(*console, keyset, journal, application, options);
            } catch (const Cancelled&) {
                stopped = true;
            }
            setSink(nullptr);
            std::string problem = console->checkConsistent(keyset);
            report(format("stopping after %d reports leaves the game whole", reports), problem.empty(), problem);
            auto stray = console->strayFiles();
            report(format("stopping after %d reports leaves nothing behind", reports), stray.empty(), stray.empty() ? "" : stray[0]);
            (void)stopped;
        }
    }

    // What was written doesn't read back right: the original has to stay.
    {
        auto console = freshConsole("corrupt");
        // Only what's read after writing: the source is read through the same calls, so the card goes bad once it has been written.
        class CorruptLater : public JobSink {
        public:
            CorruptLater(SimConsole& console) : _console(console) {}
            void progress(const char* label, i64, i64, i64) override {
                if (strstr(label, "Checking every byte"))
                    _console.setCardCorrupts(true);
            }

        private:
            SimConsole& _console;
        } corrupter(*console);
        setSink(&corrupter);
        std::string error;
        try {
            replaceAll(*console, keyset, journal, application, options);
        } catch (const ToolError& e) {
            error = e.what();
        }
        setSink(nullptr);
        console->setCardCorrupts(false);
        // The pretend console turns bad reads on at the first "Checking every byte" label, which for a game with several programs comes while
        // the originals are still being read, so the job can stop on a damaged-looking original as well. With one program it can't.
        bool saysSo = error.find("doesn't read back") != std::string::npos ||
                      (programs > 1 && error.find("the file is damaged") != std::string::npos);
        report(format("a card that reads back wrong stops the job (%d program%s)", programs, programs == 1 ? "" : "s"), saysSo, error);
        std::string problem = console->checkConsistent(keyset);
        report("and the original is still whole", problem.empty(), problem);
        auto stray = console->strayFiles();
        report("with nothing left behind", stray.empty(), stray.empty() ? "" : stray[0]);
    }

    // The power goes at every step along the way, or every so many steps for a large title.
    int cuts = 0;
    int tail = std::max(steps - 80, 1);
    for (int cut = 1; cut <= steps + 1; cut = cut >= tail ? cut + 1 : std::min(cut + stride, tail)) {
        auto console = freshConsole("cut");
        console->losePowerAt(cut);
        bool finished = false;
        try {
            replaceAll(*console, keyset, journal, application, options);
            finished = true;
        } catch (const PowerCut&) {
        }
        if (finished)
            break;
        cuts++;

        // Next start: a new program, on the same storage, and placeholders still lying where they were.
        console->losePowerAt(0);
        auto restarted = std::make_unique<SimConsole>(folder + "/cut", keyset);
        Replacer replacer(*restarted, journal);
        replacer.recover();
        sys::removeTree(options.tempDir);

        std::string problem = restarted->checkConsistent(keyset);
        report(format("power cut at step %d: the game is whole after the next start", cut), problem.empty(), problem);
        auto stray = restarted->strayFiles();
        report(format("power cut at step %d: nothing left lying around", cut), stray.empty(), stray.empty() ? "" : stray[0]);
        report(format("power cut at step %d: no journal left", cut), !sys::exists(journal + "/journal.txt"));

        // And it's as if nothing had happened: the job can just be done again.
        if (cut % 16 == 1 || stride > 1) {
            replaceAll(*restarted, keyset, journal, application, options);
            problem = restarted->checkConsistent(keyset);
            report(format("power cut at step %d: going again works", cut), problem.empty(), problem);
            stray = restarted->strayFiles();
            report(format("power cut at step %d: and leaves nothing behind", cut), stray.empty(), stray.empty() ? "" : stray[0]);
        }
    }
    printf("  power cut at %d different steps\n", cuts);

    // The system turning the journal down for a while, as a real console has been seen to with the app's own files at start-up: that's no
    // reason to take it there's none, and clear it away with the only record of a game left half replaced.
    {
        bool tried = false;
        for (int cut : {steps / 2, steps * 3 / 4, steps - 3}) {
            if (cut < 1)
                continue;
            auto console = freshConsole("refused");
            console->losePowerAt(cut);
            try {
                replaceAll(*console, keyset, journal, application, options);
            } catch (const PowerCut&) {
            }
            console->losePowerAt(0);
            std::string journalFile = journal + "/journal.txt";
            if (!sys::exists(journalFile))
                continue;
            tried = true;
            auto restarted = std::make_unique<SimConsole>(folder + "/refused", keyset);
            setTestEnv("HBP_TEST_REFUSE", journalFile);
            std::string error;
            try {
                Replacer(*restarted, journal).recover();
            } catch (const ToolError& e) {
                error = e.what();
            }
            setTestEnv("HBP_TEST_REFUSE", "");
            report(format("with the journal turned down after a power cut at step %d, recovering says it couldn't check", cut), error.find("Couldn't check") != std::string::npos, error);
            report("and the journal is still there", sys::exists(journalFile));
            Replacer(*restarted, journal).recover();
            sys::removeTree(options.tempDir);
            std::string problem = restarted->checkConsistent(keyset);
            report("once it's let through, the game is put right", problem.empty(), problem);
            report("and the journal is gone", !sys::exists(journalFile));
            break;
        }
        report("a power cut left a journal to be turned down", tried);
    }

    // The power goes while recovering from a power cut.
    for (int first : {steps / 2, steps - 3, steps - 8}) {
        if (first < 1)
            continue;
        for (int second = 1; second <= 12; second++) {
            auto console = freshConsole("cut2");
            console->losePowerAt(first);
            try {
                replaceAll(*console, keyset, journal, application, options);
            } catch (const PowerCut&) {
            }
            console->losePowerAt(0);
            auto restarted = std::make_unique<SimConsole>(folder + "/cut2", keyset);
            restarted->losePowerAt(second);
            try {
                Replacer(*restarted, journal).recover();
            } catch (const PowerCut&) {
            }
            restarted->losePowerAt(0);
            auto again = std::make_unique<SimConsole>(folder + "/cut2", keyset);
            Replacer(*again, journal).recover();
            std::string problem = again->checkConsistent(keyset);
            report(format("power cuts at steps %d and %d of recovery: whole", first, second), problem.empty(), problem);
            auto stray = again->strayFiles();
            report(format("power cuts at steps %d and %d of recovery: nothing left", first, second), stray.empty(), stray.empty() ? "" : stray[0]);
        }
    }

    // A console on an older system version starts a game whose files ask for a newer one when its installer lowered what the console's own record
    // of it says, as DBI can. The copy made in its place asks for no more than that record, besides 12.0.0 for compression, or the console
    // says it needs a system update. Nothing lowered, it asks for what the game did.
    if (!highRequired.empty() && inputs.size() >= 2) {
        constexpr u32 Wanted = 23u << 26, Lowered = 5u << 26, ForCompression = 12u << 26;
        auto required = [](SimConsole& console, u64 app, u8 type) -> u32 {
            for (auto& meta : console.describe(app).metas) {
                if (meta.key.type == type) {
                    Bytes record = console.getMeta(meta.storage, meta.key);
                    return record.size() >= 20 ? rd32(record.data() + 16) : 0;
                }
            }
            return 0;
        };
        auto lower = [](SimConsole& console, u64 app, u8 type, u32 to) {
            for (auto& meta : console.describe(app).metas) {
                if (meta.key.type == type) {
                    Bytes record = console.getMeta(meta.storage, meta.key);
                    wr32(record.data() + 16, to);
                    console.setMeta(meta.storage, meta.key, record);
                }
            }
        };
        auto fresh = [&](const std::string& name, std::vector<std::string> files) {
            std::string path = folder + "/" + name;
            sys::removeTree(path);
            sys::removeTree(journal);
            sys::removeTree(options.tempDir);
            auto console = std::make_unique<SimConsole>(path, keyset);
            console->install(files, Storage::Sd);
            return console;
        };
        {
            auto console = fresh("required_kept", {highRequired});
            u64 app = console->listApplications().at(0);
            report("a game that asks for 23.0.0, as installed, says so in its record", required(*console, app, Cnmt::MetaApplication) == Wanted,
                   format("%08x", required(*console, app, Cnmt::MetaApplication)));
            replaceAll(*console, keyset, journal, app, options);
            u32 after = required(*console, app, Cnmt::MetaApplication);
            report("compressed in place with nothing lowered, it still asks for 23.0.0", after == Wanted, format("%08x", after));
        }
        {
            auto console = fresh("required_lowered", {highRequired});
            u64 app = console->listApplications().at(0);
            lower(*console, app, Cnmt::MetaApplication, Lowered);
            replaceAll(*console, keyset, journal, app, options);
            u32 after = required(*console, app, Cnmt::MetaApplication);
            report("lowered to 5.0.0 by its installer, the copy asks for 12.0.0, for compression", after == ForCompression, format("%08x", after));
        }
        {
            // The update is the newest part, which is what the console goes by: its record asks for nothing, so the copy asks for 12.0.0.
            auto console = fresh("required_update", {highRequired, inputs[1]});
            u64 app = console->listApplications().at(0);
            replaceAll(*console, keyset, journal, app, options);
            u32 after = required(*console, app, Cnmt::MetaApplication);
            report("made one with an update whose record asks for nothing, the copy asks for 12.0.0", after == ForCompression, format("%08x", after));
            std::string problem = console->checkConsistent(keyset);
            report("and it's whole", problem.empty(), problem);
        }
    }

    printf("%d passed, %d failed\n", g_passes, g_failures);
    return g_failures > 0 ? 1 : 0;
}
