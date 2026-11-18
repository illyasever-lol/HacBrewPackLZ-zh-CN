// Problems the app finds itself, sent to the developer by itself when the person has agreed to it, as the PC versions do it (see
// src/ProblemReports.cs): a game a job couldn't do, the app stopping on something it didn't expect, and the app not answering until it was
// closed. Each problem goes once a version from a console, and after that it's only counted. Everything goes by way of a folder of reports
// waiting to go on the SD card, so no internet, or the app closing as it sends, loses nothing. The app stopping, or not answering, can't send
// anything as it happens, so that goes the next time it opens, which is also when a compressed game that stopped since gets asked about.

#include <cstdlib>
#include <ctime>

#include "app_internal.h"

#include "../core/crypto.h"

namespace hbp {

using namespace app;

namespace {

/// Not answering for less than this is only something being slow.
double freezeSeconds() {
    const char* test = getenv("HBP_TEST_FREEZE_SECONDS");
    return test && *test ? atof(test) : 30;
}

bool isWordChar(char c) {
    return isalnum((unsigned char)c) || (unsigned char)c >= 0x80;
}

bool isHexText(const std::string& text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return isxdigit((unsigned char)c) != 0; });
}

std::string firstLine(const std::string& text) {
    size_t end = text.find('\n');
    std::string line = end == std::string::npos ? text : text.substr(0, end);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
        line.pop_back();
    return line;
}

void writeWhole(const std::string& path, const std::string& text) {
    sys::File file(path, sys::File::Mode::Create);
    if (!text.empty())
        file.write(0, text.data(), text.size());
    file.flush();
}

struct Sent {
    std::string version, fingerprint;
    int count = 0;
};

}  // namespace

namespace app {

/// The words a problem is known by, without what's particular to one console's files: names in quotes, paths, title IDs and numbers, other
/// than the error codes that say which problem it is. Problems that are the same come out the same, which is what groups them.
std::string normalizedProblem(const std::string& message) {
    // A quote starts and ends a name only away from letters, as in "couldn't" or a name like 'Assassin's Creed' it's an apostrophe.
    std::string unquoted;
    for (size_t i = 0; i < message.size(); i++) {
        char c = message[i];
        if ((c == '\'' || c == '"') && (i == 0 || !isWordChar(message[i - 1]))) {
            size_t end = i + 1;
            while ((end = message.find(c, end)) != std::string::npos && end + 1 < message.size() && isWordChar(message[end + 1]))
                end++;
            if (end != std::string::npos && end > i + 1) {
                unquoted += "'...'";
                i = end;
                continue;
            }
        }
        unquoted += c;
    }

    // Then word by word: paths, title IDs, and numbers other than error codes.
    std::string out;
    size_t i = 0;
    while (i < unquoted.size()) {
        if (isspace((unsigned char)unquoted[i])) {
            if (!out.empty() && out.back() != ' ')
                out += ' ';
            i++;
            continue;
        }
        size_t end = i;
        while (end < unquoted.size() && !isspace((unsigned char)unquoted[end]))
            end++;
        std::string word = unquoted.substr(i, end - i);
        i = end;
        // What follows a word, such as a comma, isn't part of it.
        std::string after;
        while (!word.empty() && strchr(",;.)", word.back())) {
            after.insert(after.begin(), word.back());
            word.pop_back();
        }
        size_t lead = 0;
        while (lead < word.size() && word[lead] == '(')
            lead++;
        std::string before = word.substr(0, lead);
        word = word.substr(lead);
        if (word.find(":/") != std::string::npos || word.find(":\\") != std::string::npos || (word.size() > 1 && word[0] == '/' && word.find('/', 1) != std::string::npos)) {
            out += before + "..." + after;
            continue;
        }
        if (word.size() == 16 && isHexText(word)) {
            out += before + "<title ID>" + after;
            continue;
        }
        std::string shown;
        for (size_t k = 0; k < word.size();) {
            bool code = k + 9 <= word.size() && std::all_of(word.begin() + k, word.begin() + k + 4, ::isdigit) && word[k + 4] == '-' &&
                std::all_of(word.begin() + k + 5, word.begin() + k + 9, ::isdigit) && (k == 0 || !isdigit((unsigned char)word[k - 1]));
            if (code) {
                shown += word.substr(k, 9);
                k += 9;
            } else if (k + 2 < word.size() && word[k] == '0' && (word[k + 1] == 'x' || word[k + 1] == 'X') && isxdigit((unsigned char)word[k + 2])) {
                size_t hexEnd = k + 2;
                while (hexEnd < word.size() && isxdigit((unsigned char)word[hexEnd]))
                    hexEnd++;
                shown += word.substr(k, hexEnd - k);
                k = hexEnd;
            } else if (isdigit((unsigned char)word[k])) {
                while (k < word.size() && isdigit((unsigned char)word[k]))
                    k++;
                shown += '#';
            } else {
                shown += word[k++];
            }
        }
        out += before + shown + after;
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out.size() > 200 ? out.substr(0, 200) : out;
}

}  // namespace app

std::string App::problemFolder() {
    return _platform.dataFolder() + "/reports waiting";
}

/// A problem found: once a version, it's put with what's waiting to go, and after that it's only counted. Nothing is kept when sending isn't
/// wanted. A game's own name stays in the title of a game not starting, and what it's known by can be given, such as the game's title ID,
/// so that it's the same problem in any language, and however it was sent.
void App::problemFound(const std::string& kind, const std::string& message, const std::string& report, bool keepNames, const std::string& knownAs) {
    if (!_settings.sendProblems)
        return;
    std::string title = keepNames ? message.substr(0, 200) : normalizedProblem(message);
    std::string fingerprint = knownAs;
    if (fingerprint.empty()) {
        u8 digest[32];
        sha256(title.data(), title.size(), digest);
        fingerprint = kind + "|";
        for (int i = 0; i < 6; i++)
            fingerprint += format("%02x", digest[i]);
    }

    sys::Lock lock(_problemMutex);
    // Whatever goes wrong here, it mustn't stop what found the problem.
    try {
        std::string ledgerPath = _platform.dataFolder() + "/problems sent.txt";
        std::vector<Sent> ledger;
        if (sys::exists(ledgerPath)) {
            forEachLine(readTextFile(ledgerPath), [&](const std::string& line) {
                size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
                if (b != std::string::npos)
                    ledger.push_back(Sent{line.substr(0, a), line.substr(a + 1, b - a - 1), atoi(line.c_str() + b + 1)});
            });
        }
        bool seenBefore = false;
        for (auto& entry : ledger) {
            if (entry.version == AppVersion && entry.fingerprint == fingerprint) {
                entry.count++;
                seenBefore = true;
            }
        }
        if (!seenBefore)
            ledger.push_back(Sent{AppVersion, fingerprint, 0});
        std::string body = seenBefore ? "" : report.empty() ? message + "\n\n" + reportText() : report;
        std::string text;
        for (auto& entry : ledger)
            text += entry.version + "\t" + entry.fingerprint + "\t" + std::to_string(entry.count) + "\n";
        writeWhole(ledgerPath, text);
        if (!seenBefore) {
            sys::makeDirectories(problemFolder());
            std::string name = fingerprint;
            std::replace(name.begin(), name.end(), '|', '-');
            writeWhole(problemFolder() + "/" + name + ".txt",
                "kind=" + kind + "\nfingerprint=" + fingerprint + "\ntitle=" + title + "\nversion=" + AppVersion + "\n\n" + body);
        }
        logLine(std::string(seenBefore ? "Counted a problem seen before: " : "Kept a problem to send: ") + kind + ", " + title);
    } catch (const std::exception& e) {
        logLine(std::string("Couldn't keep a problem to send: ") + e.what());
    }
}

/// What's waiting goes, then how many more times the problems sent before were seen, beside the screen. With nowhere to send it, or no
/// internet, it waits for next time. When sending isn't wanted any more, it's dropped.
void App::sendWaitingProblems() {
    if (_problemsSending.exchange(true))
        return;
    _problemThread.join();
    _problemThread.start([this] {
        try {
            if (!_settings.sendProblems) {
                if (sys::exists(problemFolder()))
                    sys::removeTree(problemFolder());
                if (sys::exists(_platform.dataFolder() + "/problems sent.txt"))
                    sys::removeFile(_platform.dataFolder() + "/problems sent.txt");
            } else if (!reportAddress().empty() && _platform.isOnline()) {
                sendProblemsNow();
            }
        } catch (const std::exception& e) {
            logLine(std::string("Couldn't send the problems waiting to go: ") + e.what());
        }
        _problemsSending = false;
    }, -1, 1 << 18);
}

void App::sendProblemsNow() {
    auto keepGoing = [this] { return !_problemsStop.load(); };
    std::vector<std::string> waiting;
    if (sys::isDirectory(problemFolder())) {
        for (auto& entry : sys::list(problemFolder(), false)) {
            if (!entry.isDirectory && endsWithNoCase(entry.name, ".txt"))
                waiting.push_back(problemFolder() + "/" + entry.name);
        }
    }
    std::sort(waiting.begin(), waiting.end());
    for (auto& path : waiting) {
        std::string text = readTextFile(path);
        size_t split = text.find("\n\n");
        std::string head = split == std::string::npos ? text : text.substr(0, split);
        std::string body = split == std::string::npos ? "" : text.substr(split + 2);
        std::map<std::string, std::string> fields;
        forEachLine(head, [&](const std::string& line) {
            size_t eq = line.find('=');
            if (eq != std::string::npos)
                fields[line.substr(0, eq)] = line.substr(eq + 1);
        });
        constexpr size_t Most = 800000;
        if (body.size() > Most)
            body = "[...]\n" + body.substr(body.size() - Most);
        std::string json = "{\"app\":\"hacBrewPackLZ\",\"version\":" + jsonString(fields["version"]) + ",\"platform\":\"Switch\",\"kind\":" + jsonString(fields["kind"]) +
            ",\"fingerprint\":" + jsonString(fields["fingerprint"]) + ",\"title\":" + jsonString(fields["title"]) + ",\"said\":\"\",\"report\":" + jsonString(body) +
            ",\"device\":" + jsonString(deviceName()) + ",\"count\":1}";
        long code = 0;
        try {
            code = _platform.post(reportAddress() + "/report", json, keepGoing).first;
        } catch (const ToolError& e) {
            logLine(std::string("A problem couldn't be sent yet: ") + e.what());
            return;
        }
        // Turned away for now, it's tried again next time. Turned away for what it is, sending it again won't change that.
        if (code == 429 || code >= 500)
            return;
        logLine(format("Sent a problem: %s (%ld)", fields["title"].c_str(), code));
        sys::removeFile(path);
    }

    // Then the counts, each taken off once it's dealt with, as more may have been seen meanwhile.
    std::string ledgerPath = _platform.dataFolder() + "/problems sent.txt";
    std::vector<Sent> counts;
    {
        sys::Lock lock(_problemMutex);
        if (sys::exists(ledgerPath)) {
            forEachLine(readTextFile(ledgerPath), [&](const std::string& line) {
                size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
                if (b != std::string::npos && atoi(line.c_str() + b + 1) > 0)
                    counts.push_back(Sent{line.substr(0, a), line.substr(a + 1, b - a - 1), atoi(line.c_str() + b + 1)});
            });
        }
    }
    for (auto& seen : counts) {
        std::string json = "{\"app\":\"hacBrewPackLZ\",\"version\":" + jsonString(seen.version) + ",\"fingerprint\":" + jsonString(seen.fingerprint) +
            ",\"device\":" + jsonString(deviceName()) + format(",\"count\":%d}", std::min(seen.count, 1000));
        long code = 0;
        try {
            code = _platform.post(reportAddress() + "/seen", json, keepGoing).first;
        } catch (const ToolError&) {
            return;
        }
        if (code != 200 && code != 404)
            return;
        sys::Lock lock(_problemMutex);
        std::string text;
        forEachLine(readTextFile(ledgerPath), [&](const std::string& line) {
            size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
            if (b == std::string::npos)
                return;
            int count = atoi(line.c_str() + b + 1);
            if (line.substr(0, a) == seen.version && line.substr(a + 1, b - a - 1) == seen.fingerprint)
                count = std::max(count - seen.count, 0);
            text += line.substr(0, b) + "\t" + std::to_string(count) + "\n";
        });
        writeWhole(ledgerPath, text);
    }
}

/// What the last time left: a note that it had stopped answering, which it never came out of, or the app having stopped on something it
/// didn't expect, which Atmosphère will have written up. Either goes with what's waiting. Then this time is noted, and the watch starts.
void App::checkLastTime() {
    std::string folder = _platform.dataFolder();
    std::string running = folder + "/running.txt";
    std::string frozen = folder + "/not answering.txt";
    long long now = (long long)time(nullptr);
    try {
        long long openedBefore = sys::exists(running) ? std::max(atoll(readTextFile(running).c_str()), 1LL) : 0;
        if (sys::exists(frozen)) {
            std::string note = readTextFile(frozen);
            sys::removeFile(frozen);
            problemFound("freeze", firstLine(note), note);
        } else if (openedBefore > 0) {
            // Atmosphère names each report for when it happened, in seconds, then what crashed.
            std::string newest, newestPath;
            for (auto& crashes : _platform.crashReportFolders()) {
                if (!sys::isDirectory(crashes))
                    continue;
                for (auto& entry : sys::list(crashes, false)) {
                    if (entry.isDirectory || !endsWithNoCase(entry.name, ".log") || atoll(entry.name.c_str()) + 5 < openedBefore || entry.name <= newest)
                        continue;
                    std::string contents = readTextFile(crashes + "/" + entry.name);
                    if (contents.find("(hacBrewPackLZ +") == std::string::npos && contents.find("Name:                    hacBrewPackLZ") == std::string::npos)
                        continue;
                    newest = entry.name;
                    newestPath = crashes + "/" + entry.name;
                }
            }
            if (!newestPath.empty()) {
                // Only what says where it stopped and why: no registers, which can hold part of a key.
                std::string contents = readTextFile(newestPath), result, type, where, lines;
                forEachLine(contents, [&](const std::string& line) {
                    std::string trimmed = line.substr(std::min(line.find_first_not_of(' '), line.size()));
                    size_t own = trimmed.find("(hacBrewPackLZ +");
                    if (trimmed.rfind("Result:", 0) == 0 && result.empty()) {
                        size_t code = trimmed.find('(');
                        result = code == std::string::npos ? "" : trimmed.substr(code + 1, trimmed.find(')', code) - code - 1);
                        lines += trimmed + "\n";
                    } else if (trimmed.rfind("Type:", 0) == 0 && type.empty()) {
                        size_t value = trimmed.find_first_not_of(' ', 5);
                        type = value == std::string::npos ? "" : trimmed.substr(value);
                        lines += trimmed + "\n";
                    } else if (own != std::string::npos) {
                        std::string place = trimmed.substr(own + 1, trimmed.find(')', own) - own - 1);
                        if (trimmed.rfind("PC:", 0) == 0)
                            where = place;
                        lines += trimmed.substr(0, trimmed.find(':') + 1) + " " + place + "\n";
                    }
                });
                problemFound("crash", "Stopped: " + (type.empty() ? std::string("a crash") : type) + (where.empty() ? "" : " at " + where) +
                    (result.empty() ? "" : " (" + result + ")"), "What Atmosph\xC3\xA8re says of it:\n" + lines + "\n" + reportText());
            }
        }
        _lastOpened = sys::exists(folder + "/last opened.txt") ? atoll(readTextFile(folder + "/last opened.txt").c_str()) : 0;
        writeWhole(folder + "/last opened.txt", std::to_string(now));
        writeWhole(running, std::to_string(now));
    } catch (const std::exception& e) {
        // Whatever it was, it mustn't stop the app opening.
        logLine(std::string("Couldn't look at how the last time ended: ") + e.what());
    }
    sendWaitingProblems();
    watchForFreezes();
}

/// Watches the screen being drawn, beside it. Once it hasn't been for a while, a note says what the app was doing, which only becomes a report
/// if the app is then closed without coming out of it: a card that's slow for a moment isn't a problem.
void App::watchForFreezes() {
    _aliveAt = sys::now();
    _watchStop = false;
    _watchThread.start([this] {
        std::string note = _platform.dataFolder() + "/not answering.txt";
        double limit = freezeSeconds();
        bool noted = false;
        // How long this has seen it quiet itself, a quarter of a second at a time. The clock can jump, as it may over the console's sleep,
        // and the system's keyboard holds up the screen for as long as it's open, and neither of those is the app not answering.
        double watchedQuiet = 0;
        while (!_watchStop) {
            sys::sleepMs(250);
            double quiet = sys::now() - _aliveAt.load();
            if (_watchPaused || quiet < std::min(5.0, limit)) {
                watchedQuiet = 0;
                if (noted) {
                    sys::removeFile(note);
                    noted = false;
                }
                continue;
            }
            watchedQuiet += 0.25;
            if (noted || watchedQuiet < limit || !_settings.sendProblems)
                continue;
            noted = true;
            static const char* const Doing[] = {"nothing in particular", "starting", "looking through the lists", "compressing", "extracting", "checking", "removing", "fixing"};
            std::string text = std::string("Stopped answering while ") + Doing[(int)_jobKind.load()] + "\n\n";
            // Only if it can be had straight away: what's stuck may be holding it.
            if (_logMutex.tryLock()) {
                text += format("The last %d lines of what the app said:\n", (int)_recentLog.size());
                for (auto& line : _recentLog)
                    text += line;
                _logMutex.unlock();
            }
            try {
                writeWhole(note, text);
            } catch (const std::exception&) {
            }
        }
    }, -1, 1 << 16);
}

/// Closing as it should: nothing about this time needs telling next time.
void App::closingProblems() {
    _problemsStop = true;
    _watchStop = true;
    _watchThread.join();
    _problemThread.join();
    sys::removeFile(_platform.dataFolder() + "/running.txt");
    sys::removeFile(_platform.dataFolder() + "/not answering.txt");
}

/// A compressed game that stopped since this app was last open is asked about: whether it started. Atmosphère says when one stopped with an
/// error, and the console's play history when one closed within moments of starting, which leaves nothing for Atmosphère to write down, as with
/// the system's error screen or a game that closes by itself. Asked once the games it could be are known to be compressed or not, which takes
/// analyzing them, about the first one, and only once.
void App::askAboutStoppedGames() {
    if (_lastOpened <= 0 || !_console) {
        _askedAboutGames = true;
        return;
    }
    // What stopped since, read the once. Atmosphère names each report for when it happened, in seconds, then the program that crashed.
    if (!_stoppedListed) {
        _stoppedListed = true;
        for (auto& crashes : _platform.crashReportFolders()) {
            try {
                if (!sys::isDirectory(crashes))
                    continue;
                for (auto& entry : sys::list(crashes, false)) {
                    size_t underscore = entry.name.find('_');
                    // After the app last opened, not in that same second, so what was asked about then isn't asked about again.
                    if (!entry.isDirectory && endsWithNoCase(entry.name, ".log") && atoll(entry.name.c_str()) > _lastOpened && underscore != std::string::npos)
                        _stoppedPrograms.push_back({strtoull(entry.name.c_str() + underscore + 1, nullptr, 16), entry.name, 0});
                }
            } catch (const ToolError&) {
            }
        }
        // Then what the play history has starting since, after that same second too, and closing within moments: a game that ran longer started.
        constexpr long long ClosedSoon = 20;
        for (auto& played : _platform.gamesPlayedSince(_lastOpened)) {
            if (played.ended >= played.started && played.ended - played.started <= ClosedSoon)
                _stoppedPrograms.push_back({played.application, "", played.ended - played.started});
        }
    }
    struct Stopped {
        std::string name;
        u64 application = 0;
        std::string crash;
        long long ranFor = 0;
    };
    std::vector<Stopped> stopped;
    {
        sys::Lock lock(_mutex);
        for (auto& program : _stoppedPrograms) {
            for (auto& row : _rows) {
                if (row.tab != TabInstalled || (row.application & ~0xFFFULL) != (program.program & ~0xFFFULL))
                    continue;
                // Not known yet: asked again in a moment, for a while.
                if (!row.hasEstimate && row.state != RowState::Done && row.state != RowState::Failed && _time < 180)
                    return;
                if (isCompressed(row) && std::none_of(stopped.begin(), stopped.end(), [&](const Stopped& s) { return s.application == row.application; }))
                    stopped.push_back(Stopped{row.name, row.application, program.crash, program.ranFor});
            }
        }
    }
    _askedAboutGames = true;
    if (stopped.empty())
        return;
    const Stopped& game = stopped.front();
    bool crashed = !game.crash.empty();
    std::string howSoon = game.ranFor <= 0 ? "as soon as it started" : game.ranFor == 1 ? "a second after it started" : format("%lld seconds after it started", game.ranFor);
    // v25：日志仍用英文 howSoon，对话框正文改用中文时长短语。
    std::string howSoonZh = game.ranFor <= 0 ? "刚启动便" : game.ranFor == 1 ? "启动 1 秒后便" : format("启动 %lld 秒后便", game.ranFor);
    if (crashed)
        logLine(format("%s stopped with an error since last time (%s)", game.name.c_str(), game.crash.c_str()));
    else
        logLine(format("%s closed %s since last time, as the console's play history has it", game.name.c_str(), howSoon.c_str()));
    auto dialog = std::make_unique<Dialog>();
    dialog->title = game.name + " 启动了吗？";
    dialog->paragraphs.push_back(crashed ? std::string("上次打开本程序后，该游戏因出错而停止，Atmosph\xC3\xA8re 已记录下错误信息。它经过压缩，可能与压缩有关。")
                                         : "上次打开本程序后，主机的游玩记录显示它" + howSoonZh + "退出了。它经过压缩，可能与压缩有关。");
    if (stopped.size() == 2)
        dialog->paragraphs.push_back("另有一个已压缩的游戏也停止了。");
    else if (stopped.size() > 2)
        dialog->paragraphs.push_back(format("另有 %d 个已压缩的游戏也停止了。", (int)stopped.size() - 1));
    std::string name = game.name;
    u64 application = game.application;
    dialog->choices.push_back({"已启动", nullptr});
    dialog->choices.push_back({"未能启动", [this, name, application, crashed] { gameDidntStart(name, application, crashed); }});
    dialog->cancelChoice = 0;
    _dialog = std::move(dialog);
    _dirty = true;
}

/// What usually helps, and what happened goes to the developer: by itself when that's agreed to, or when the person says so. It goes with what
/// Atmosphère wrote down when the game crashed, or with how long it ran, which the app's log says.
void App::gameDidntStart(const std::string& name, u64 application, bool crashed) {
    std::string title = format("%s [%016llX] didn't start after being compressed", name.c_str(), (unsigned long long)application);
    std::string fingerprint = format("game-didnt-start|%016llx", (unsigned long long)application);
    auto dialog = std::make_unique<Dialog>();
    dialog->title = "常见的解决办法";
    dialog->paragraphs.push_back("官方更新补丁直接装到压缩游戏上会使其无法启动，直到把补丁整合进去：在"
        "「已安装游戏」中压缩该游戏即可。压缩游戏还需要签名补丁及 12.0.0 以上系统版本。");
    std::string along = crashed ? "报告中会附上 Atmosph\xC3\xA8re 记录的错误信息，不会包含你的密钥。" : "报告中会附上运行时长，不会包含你的密钥。";
    if (_settings.sendProblems) {
        problemFound("game-didnt-start", title, title + "\n\n" + reportText(), true, fingerprint);
        sendWaitingProblems();
        dialog->paragraphs.push_back("问题详情将发送给开发者，" + along);
        dialog->choices.push_back({"确定", nullptr});
    } else {
        dialog->paragraphs.push_back("把问题详情发送给开发者有助于定位原因，" + along);
        // In the one press, with what Atmosphère wrote down or how long it ran: there's nothing more to say than that it didn't start.
        dialog->choices.push_back({"发送", [this, fingerprint, title] { reportProblem("game-didnt-start", fingerprint, title, false); }});
        dialog->choices.push_back({"暂不", nullptr});
    }
    dialog->cancelChoice = (int)dialog->choices.size() - 1;
    _dialog = std::move(dialog);
    _dirty = true;
}

App::OptionItem App::sendProblemsOption(const char* section) {
    return OptionItem{section, "发送问题报告",
                      "出问题时会自动向开发者发送报告以便修复，报告中绝不包含你的密钥。",
                      [this] { return std::string(_settings.sendProblems ? "开" : "关"); }, [this](int) {
                          _settings.sendProblems = !_settings.sendProblems;
                          saveSettings();
                          // Turned off, whatever was waiting to go is dropped.
                          sendWaitingProblems();
                      }};
}

/// Asked the once, of whoever had the welcome before sending problems was on it: whether to. Either answer is kept, B as no, and it's never
/// asked again.
void App::askAboutSendingProblems() {
    auto answer = [this](bool send) {
        _settings.sendProblems = send;
        _settings.problemsAsked = true;
        saveSettings();
        _dirty = true;
    };
    auto dialog = std::make_unique<Dialog>();
    dialog->title = "发送问题报告？";
    dialog->paragraphs.push_back("出问题时可自动向开发者发送报告以便修复，报告中绝不包含你的密钥。");
    dialog->paragraphs.push_back("之后可在「设置 → 更多设置」中修改。");
    dialog->choices.push_back({"发送", [answer] { answer(true); }});
    dialog->choices.push_back({"暂不", [answer] { answer(false); }});
    dialog->cancelChoice = 1;
    _dialog = std::move(dialog);
    _dirty = true;
}

}  // namespace hbp
