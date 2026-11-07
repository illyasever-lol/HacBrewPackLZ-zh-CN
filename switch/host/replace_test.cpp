// Replaces installed games on the pretend console, and pulls the plug at every step along the way.
//   replace_test -k keys folder base.nsp update.nsp [dlc.nsp...]
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

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 1 << 16);
    std::string keys, folder;
    std::vector<std::string> inputs;
    int stride = 1;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-k")
            keys = argv[++i];
        else if (arg == "--stride")
            stride = atoi(argv[++i]);
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

    // The plain case, and how many steps it takes.
    u64 application;
    int steps;
    i64 before;
    {
        auto console = freshConsole("plain");
        auto list = console->listApplications();
        report("the game is installed", list.size() == 1);
        application = list.at(0);
        before = console->describe(application).totalBytes();
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
        auto console = freshConsole("已拒绝");
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
        report("a card that reads back wrong stops the job", error.find("doesn't read back") != std::string::npos, error);
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

    printf("%d passed, %d failed\n", g_passes, g_failures);
    return g_failures > 0 ? 1 : 0;
}
