// Help: a few lines on what people most often need to know, what to try next when something fails, a report of what happened
// that can be sent without any keys in it, and spotting an official update installed over a game this tool rebuilt.
#include <ctime>

#include "app_internal.h"

#include "../core/nca.h"

namespace hbp {

using namespace app;

namespace app {

// The same topics as docs/help.md, which says more, and which each one's code leads to.
const HelpTopic HelpTopics[] = {
    {"游戏无法启动",
     "压缩游戏需要签名补丁，且系统版本不低于 12.0.0；否则主机会提示游戏已损坏或要求系统更新。\n\n官方更新补丁直接装到压缩游戏上会使其无法启动：补丁只适配原始游戏。该游戏会列在「已安装游戏」中，压缩它即可把补丁整合进去。\n\n1.4.3 及更早版本由「本体＋补丁」生成的成品，可能仍要求更新而不启动；1.5.0 test 2 及更早版本处理过、由 DBI / sphaira 在旧系统上可启动的游戏也可能如此。概览页会提示，用「修复」即可。\n\n若都不是，请连同游戏与错误码报告问题。",
    "a-game-wont-start"},
    {"为压缩游戏添加更新补丁",
     "更新补丁必须整合进压缩或重新打包的游戏文件中，直接单独安装会导致游戏无法启动。\n\n在电脑或手机上：把补丁与压缩版游戏放在一起，点「压缩」后安装新成品即可。\n\n在 Switch 上：正常装完补丁后，游戏会出现在「已安装游戏」，按 X 即可整合；也可把补丁放进 SD 卡或由电脑共享，在「游戏文件」中按 X 整合。\n\n一般无需原始游戏文件，\n1.5 版之前压缩的或开发者压缩的游戏除外，程序会提示。\n\n存档不会丢失。要阻止主机自动更新，请在\n「系统设置 → 系统」中关闭「软件自动更新」。",
    "updating-a-game"},
    {"缺少密钥",
     "「密钥集中不存在」表示密钥比游戏更旧。请升级系统版本，重新运行 Lockpick_RCM，并使用新导出的 prod.keys（sdmc:/switch/prod.keys）。\n缺少密钥的游戏会在每次启动时重试。",
    "missing-keys"},
    {"缺少票据",
     "「需要标题密钥」或「个性化票据」表示没有游戏票据。请使用含票据的游戏副本；在本机购买的游戏，其票据位于购买时的系统（sysMMC 或 emuMMC）上，请在该系统中打开本程序。",
    "missing-tickets"},
    {"为什么有些游戏几乎压不动",
     "许多新游戏本身已打包得很紧，或把大部分数据放在压缩覆盖不到的位置。「分析」会在处理前算出每个游戏能节省的空间；实测低于设置中「最小节省比例」的游戏会归入「已完成」并说明原因。",
    "games-that-barely-shrink"},
    {"把游戏发送到 Switch",
     "在电脑上运行「发送到 Switch」，或在 Android 应用中使用，与 Switch 处于同一 Wi-Fi。游戏会出现在「游戏文件」中：勾选后按 X。也可用 USB 数据线：Windows 需 Zadig 安装 WinUSB 驱动，Linux / Mac 需 libusb，详见电脑端帮助。",
    "sending-games-to-the-switch"},
    {"文件都放在哪里",
     "本程序保存的 NSP 放在 sdmc:/hacBrewPackLZ，除非在设置中另选位置；「查看内部」保存的内容放在其中的「游戏文件」文件夹。日志为 sdmc:/switch/hacBrewPackLZ/log.txt（若主机不允许该文件名则为 log-other.txt），报告放在旁边。「游戏文件」不搜索系统文件夹和 switch 深处各应用自己的目录；+ 菜单中的「搜索位置」可指定并逐层查看某个文件夹。",
    "where-things-are"},
};
const size_t HelpTopicCount = sizeof(HelpTopics) / sizeof(HelpTopics[0]);

/// What the file system's own error code (2002-NNNN) in a failure means, where nothing in its words says: each number, or range of them, as
/// the system's list of them has it. The first code with a meaning goes; null if there's none. As Help.FileSystemCodeReason on the PC.
static const char* plainForFileSystemCode(const std::string& failure) {
    for (size_t at = failure.find("(2002-"); at != std::string::npos; at = failure.find("(2002-", at + 6)) {
        if (at + 11 > failure.size() || failure[at + 10] != ')')
            continue;
        int code = 0;
        bool digits = true;
        for (size_t i = at + 6; i < at + 10; i++) {
            digits = digits && failure[i] >= '0' && failure[i] <= '9';
            code = code * 10 + (failure[i] - '0');
        }
        if (!digits)
            continue;
        if (code == 1)
            return "所需的某个文件已不存在。";
        if (code == 7)
            return "所需的某个文件被占用，请重试。";
        if (code >= 30 && code <= 45)
            return "可用空间不足。";
        if (code == 1004)
            return "该游戏缺少票据。";
        if (code >= 2000 && code <= 2499)
            return "无法访问 SD 卡，请确认已正确插入。";
        if (code >= 2500 && code <= 2999)
            return "无法读取游戏卡，请确认已正确插入。";
        if (code >= 3200 && code <= 3499)
            return "内存不足。";
        if (code >= 3500 && code <= 3999)
            return "无法访问主机自带存储，请重启主机。";
        // The SD card's FAT itself, before the rest of what says data is corrupted.
        if (code >= 4681 && code <= 4699)
            return "SD 卡文件系统已损坏，请在电脑上检查。";
        if (code >= 4000 && code <= 4999)
            return "已损坏：请重新转储或下载。";
    }
    return nullptr;
}

std::string plainReason(const std::string& failure) {
    auto has = [&](const char* text) { return failure.find(text) != std::string::npos; };
    // The card's own trouble first, as some of it says "damaged" too.
    if (has("file system it was writing to is damaged"))
        return "SD 卡文件系统已损坏，请在电脑上检查。";
    if (has("(2001-0123)"))
        return "SD 卡无响应，请重新打开程序。";
    if (has("(2001-0106)") || has("(2001-0105)"))
        return "SD 卡正忙，请重试。";
    // Before anything the firmware's own error, which follows, could be taken for (Replacer::checkConsole, checkNewCopy). The words before 1.6
    // too, as a report from an older version still says them.
    if (has("can't start compressed games yet") || has("firmware won't read compressed games"))
        return "这台主机目前还无法启动压缩游戏。";
    if (has("doesn't read back the same"))
        return "SD 卡可能即将损坏，请在电脑上检查。";
    if (has("differently from"))
        return "新生成的文件不正确，因此保留原始文件。";
    if (has("isn't a readable NCA3") || has("header_key"))
        return "你的密钥无法打开这个游戏。";
    if (has("not in your keys") || has("not in the keyset"))
        return "你的密钥对这个游戏来说太旧了。";
    if (has("needs a title key") || has("personalized ticket"))
        return "该游戏缺少票据。";
    // A hash check that fails where an update is laid over its game can be either, as the failure says, so this says so too.
    if (has("doesn't belong to this base game"))
        return "文件已损坏，或更新补丁与本体版本不匹配：请两者都检查。";
    // An update over a game compressed or repacked with an earlier update in it and no map, which that update's file or the original game makes
    // up for (checkUpdateFits). A saved error from before can still say it "doesn't fit it by itself".
    if (has("in it already, so it doesn't have all of the original game") || has("doesn't fit it by itself"))
        return "This update needs the earlier update, or the original game.";
    // A game compressed or repacked that doesn't have all of the original game an update needs. Saved errors from before can still say "the
    // compressed copy doesn't have", or that the update "doesn't fit the compressed copy".
    if (has("compressed game doesn't have") || has("repacked game doesn't have") || has("game that's installed doesn't have") ||
        has("compressed copy doesn't have") || has("doesn't fit the compressed copy"))
        return "这个更新补丁需要原始本体游戏。";
    if (has("can't be added to the game that's installed") || has("doesn't fit the game that's installed"))
        return "The game is damaged, or this update is for another version.";
    if (has("enough room") || has("not enough space"))
        return "可用空间不足。";
    if (has("enough memory") || has("enough free memory"))
        return "内存不足。";
    if (has("can't be found any more"))
        return "共享该文件的电脑已停止共享。";
    // The system's own code, where the words don't say.
    if (const char* coded = plainForFileSystemCode(failure))
        return coded;
    // Anything else that stops a DLC going into its game's file (saveNsps), as on the PC. With the setting off, the game is made without it.
    if (failure.rfind("Its DLC", 0) == 0)
        return "某个 DLC 已损坏：请重新获取，或关闭「游戏与 DLC 打包在同一文件」。";
    if (has("damaged") || has("hash check failed") || has("doesn't match") || has("lies outside") || has("cut short") || has("BKTR") ||
        has("ended earlier than expected"))
        return "已损坏：请重新转储或下载。";
    return "Something went wrong. Its details say what.";
}

std::string withNextStep(const std::string& failure) {
    auto has = [&](const char* text) { return failure.find(text) != std::string::npos; };
    const char* next = nullptr;
    if (has("isn't a readable NCA3") || has("header_key"))
        next = "Your keys file may not be this console's, or may be damaged: dump it again with Lockpick_RCM.";
    else if (has("not in your keys"))
        next = "Update the console's system version, then dump your keys again with Lockpick_RCM.";
    else if (has("needs a title key") || has("personalized ticket"))
        next = "Use a copy of the game that has its ticket, or open this app on the system the game was bought on.";
    // "a part", as a game compressed or repacked says when it doesn't have one an update reads, in this wording or the one before 1.5. An update
    // that can't be made part of an installed game says "parts", with what to do already.
    else if (has("doesn't belong to this base game") || has("a part of the original game that the"))
        next = "If the game was compressed or repacked with an earlier update in it before version 1.5, or its developer compressed it, the original game is needed for this update.";
    else if (has("BKTR") || has("damaged") || has("doesn't match") || has("lies outside the file") || has("cut short"))
        next = "请重新转储或下载，并确认更新补丁与该游戏版本匹配。";
    else if (has("Something unexpected went wrong"))
        next = "Please report it, from + and then Help.";
    if (!next || has(next))
        return failure;
    std::string text = failure;
    while (!text.empty() && (text.back() == '.' || text.back() == ' '))
        text.pop_back();
    return text + ". " + next;
}

}  // namespace app

namespace {

/// Every key's value taken out, whatever the case it's in, which nothing here should ever have written down, but a report
/// is sent to someone else, so it's made sure of.
std::string withoutKeys(std::string text, const std::vector<std::string>& secrets) {
    std::set<std::string> known(secrets.begin(), secrets.end());
    std::set<size_t> lengths;
    for (auto& secret : secrets)
        lengths.insert(secret.size());
    auto isHex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
    size_t i = 0;
    while (i < text.size()) {
        if (!isHex(text[i])) {
            i++;
            continue;
        }
        size_t end = i;
        while (end < text.size() && isHex(text[end]))
            end++;
        std::string run = text.substr(i, end - i);
        for (auto& c : run)
            c = (char)tolower((unsigned char)c);
        bool found = false;
        for (size_t length : lengths) {
            for (size_t at = 0; at + length <= run.size() && !found; at++) {
                if (known.count(run.substr(at, length))) {
                    text.replace(i, end - i, "[已省略密钥]");
                    end = i + strlen("[已省略密钥]");
                    found = true;
                }
            }
            if (found)
                break;
        }
        i = end;
    }
    return text;
}

/// Every 8 bytes of every key, as a register would show them either way round, which is how much of a key a crash report's
/// registers could hold. Only stretches that look like key, as some keys hold long runs of zeros, which ordinary numbers would match.
std::set<std::string> keyHalves(const std::vector<std::string>& secrets) {
    std::set<std::string> halves;
    for (auto& secret : secrets) {
        for (size_t at = 0; at + 16 <= secret.size(); at += 2) {
            std::string forward = secret.substr(at, 16);
            std::set<std::string> different;
            int zeros = 0;
            for (size_t i = 0; i < 16; i += 2) {
                different.insert(forward.substr(i, 2));
                zeros += forward.compare(i, 2, "00") == 0 ? 1 : 0;
            }
            if (different.size() < 5 || zeros > 2)
                continue;
            std::string backward;
            for (size_t i = 16; i >= 2; i -= 2)
                backward += forward.substr(i - 2, 2);
            halves.insert(forward);
            halves.insert(backward);
        }
    }
    return halves;
}

/// Atmosphère's own reports of what crashed lately, the newest few. What they copy out of memory is left out, as memory could
/// hold anything, and so is any register that holds part of a key.
std::string crashReports(const std::vector<std::string>& folders, const std::vector<std::string>& secrets) {
    constexpr size_t Newest = 5;
    constexpr long long MonthSeconds = 30LL * 24 * 3600;
    constexpr size_t MostLines = 300;
    struct Found {
        long long when;
        std::string path, name;
    };
    std::vector<Found> found;
    long long now = (long long)time(nullptr);
    for (auto& folder : folders) {
        try {
            if (!sys::isDirectory(folder))
                continue;
            for (auto& entry : sys::list(folder, false)) {
                if (entry.isDirectory || !endsWithNoCase(entry.name, ".log"))
                    continue;
                // Named for when it happened, in seconds, then what crashed.
                long long when = atoll(entry.name.c_str());
                bool dated = when > 1000000000LL && when < 4000000000LL;
                if (dated && now > 1000000000LL && now - when > MonthSeconds)
                    continue;
                found.push_back(Found{when, joinPath(folder, entry.name), fileNameOf(folder) + "/" + entry.name});
            }
        } catch (const ToolError&) {
        }
    }
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.when > b.when; });
    if (found.size() > Newest)
        found.resize(Newest);

    std::set<std::string> halves = keyHalves(secrets);
    auto isHex = [](char c) { return isxdigit((unsigned char)c) != 0; };
    std::string text;
    for (auto& report : found) {
        std::string contents;
        try {
            contents = readTextFile(report.path);
        } catch (const ToolError& e) {
            text += "--- " + report.name + ": couldn't be read, " + e.what() + "\n";
            continue;
        }
        text += "--- " + report.name + "\n";
        size_t lines = 0, dumped = 0;
        forEachLine(contents, [&](std::string line) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            // A line of memory is bytes, two hex digits each, a row of them.
            int bytes = 0;
            for (size_t i = 0; i + 1 < line.size(); i++) {
                if (isHex(line[i]) && isHex(line[i + 1]) && (i == 0 || line[i - 1] == ' ') && (i + 2 == line.size() || line[i + 2] == ' '))
                    bytes++;
            }
            if (bytes >= 8) {
                dumped++;
                return;
            }
            if (lines >= MostLines)
                return;
            // Registers are 16 hex digits.
            for (size_t i = 0; i < line.size();) {
                if (!isHex(line[i])) {
                    i++;
                    continue;
                }
                size_t end = i;
                while (end < line.size() && isHex(line[end]))
                    end++;
                std::string run = line.substr(i, end - i);
                for (auto& c : run)
                    c = (char)tolower((unsigned char)c);
                if (run.size() == 16 && halves.count(run)) {
                    line.replace(i, end - i, "[left out]");
                    end = i + strlen("[left out]");
                }
                i = end;
            }
            text += line + "\n";
            lines++;
        });
        if (dumped > 0)
            text += format("(%d lines copied out of memory left out)\n", (int)dumped);
    }
    return found.empty() ? "近一个月没有记录。\n" : text;
}

const char* stateName(int state) {
    static const char* const names[] = {"ready", "working", "done", "failed"};
    return state >= 0 && state < 4 ? names[state] : "?";
}

}  // namespace

void App::showHelpTopic(size_t index) {
    if (index >= HelpTopicCount)
        return;
    const HelpTopic& topic = HelpTopics[index];
    auto dialog = std::make_unique<Dialog>();
    dialog->title = topic.title;
    forEachLine(topic.text, [&](const std::string& line) {
        if (!line.empty())
            dialog->paragraphs.push_back(line);
    });
    dialog->qr = std::string(HelpPage) + "#" + topic.anchor;
    dialog->qrCaption = "手机扫码查看更多";
    dialog->choices = {{"确定", nullptr}};
    dialog->cancelChoice = 0;
    _menu.reset();
    _dialog = std::move(dialog);
    _dirty = true;
}

namespace app {

/// Where reports go: the report service in report-service/. With no address, reports are saved on the SD card.
constexpr const char* ReportServiceUrl = "https://hacbrewpacklz-reports.hacbrewpacklz.workers.dev";

/// Somewhere else to send them, for testing, or "none" for nowhere.
std::string reportAddress() {
    const char* test = getenv("HBP_TEST_REPORT_SITE");
    if (test && std::string(test) == "none")
        return "";
    return test && *test ? test : ReportServiceUrl;
}

std::string jsonString(const std::string& text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\')
            out += '\\', out += (char)c;
        else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else if (c == '\t')
            out += "\\t";
        else if (c < 0x20)
            out += format("\\u%04x", c);
        else
            out += (char)c;
    }
    return out + "\"";
}

/// A string or a number from a reply, which is all there is to them.
std::string jsonValue(const std::string& json, const char* name) {
    std::string key = std::string("\"") + name + "\":";
    size_t at = json.find(key);
    if (at == std::string::npos)
        return "";
    at += key.size();
    if (at < json.size() && json[at] == '"') {
        size_t end = json.find('"', at + 1);
        return end == std::string::npos ? "" : json.substr(at + 1, end - at - 1);
    }
    size_t end = json.find_first_of(",}", at);
    return json.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

}  // namespace app

/// A name this copy made up for itself the first time, so the service can tell one console from another and nothing more.
std::string App::deviceName() {
    std::string path = _platform.dataFolder() + "/device.txt";
    if (sys::exists(path)) {
        std::string known = readSaved("device.txt");
        while (!known.empty() && (known.back() == '\n' || known.back() == '\r'))
            known.pop_back();
        if (known.size() == 32)
            return known;
    }
    u8 bytes[16];
    sys::randomBytes(bytes, sizeof(bytes));
    std::string made;
    for (u8 b : bytes)
        made += format("%02x", b);
    try {
        sys::File file(path, sys::File::Mode::Create);
        file.write(0, made.data(), made.size());
        file.flush();
    } catch (const ToolError&) {
    }
    return made;
}

/// Straight to the developer, with what the person typed, and what came of it said once it's back. With nowhere to send it, or no internet,
/// it's saved on the SD card instead.
void App::reportProblem(const std::string& kind, const std::string& fingerprint, const std::string& about, bool askWhatHappened) {
    if (_reporting)
        return;
    if (!askWhatHappened) {
        sendReport(kind, fingerprint, about, "");
        return;
    }
    // The system's keyboard holds up the screen for as long as it's open, which isn't the app not answering.
    _watchPaused = true;
    std::optional<std::string> typed = _platform.typeText("当时在做什么？哪个游戏，按了哪个键", 500);
    _aliveAt = sys::now();
    _watchPaused = false;
    // Cancelling the keyboard cancels the report.
    if (!typed)
        return;
    // Then it's asked, with B as no. The keyboard's own buttons aren't the ones anyone goes by, and B on it had the report sent.
    std::string said = *typed;
    bool sending = !reportAddress().empty() && _platform.isOnline();
    auto dialog = std::make_unique<Dialog>();
    dialog->title = sending ? "发送此报告？" : "保存此报告？";
    dialog->paragraphs.push_back(sending ? "报告会直接发送给开发者，包含版本、设置、列表与程序输出，不含密钥。"
                                         : "报告会保存到 SD 卡，包含版本、设置、列表与程序输出，不含密钥。");
    if (!said.empty()) {
        // Not cut in the middle of a letter.
        size_t most = said.size();
        if (most > 200) {
            most = 200;
            while (most > 0 && ((unsigned char)said[most] & 0xC0) == 0x80)
                most--;
        }
        dialog->paragraphs.push_back("你填写的内容：" + said.substr(0, most) + (most < said.size() ? "..." : ""));
    }
    dialog->choices.push_back({sending ? "Send it" : "保存", [this, kind, fingerprint, about, said] { sendReport(kind, fingerprint, about, said); }});
    dialog->choices.push_back({"取消", nullptr});
    dialog->cancelChoice = 1;
    _menu.reset();
    _dialog = std::move(dialog);
    _dirty = true;
}

/// What's in a report goes to the developer, or is saved where it can be posted from, with what was typed first.
void App::sendReport(const std::string& kind, const std::string& fingerprint, const std::string& about, const std::string& said) {
    if (_reporting)
        return;
    std::string report = reportText();
    if (reportAddress().empty() || !_platform.isOnline()) {
        saveReport(said);
        return;
    }
    _reporting = true;
    _reportThread.join();
    showMessage("正在发送报告", "报告会直接发送给开发者，包含版本、设置、列表与程序输出，不含密钥。"
        "收到回复后会在此显示结果。");
    _reportThread.start([this, said, report, kind, fingerprint, about] {
        constexpr size_t Most = 800000;
        std::string body = report.size() > Most ? "[...]\n" + report.substr(report.size() - Most) : report;
        std::string title = !about.empty() ? about : said.empty() ? std::string("来自 Switch 的报告") : said.substr(0, 80);
        std::string json = "{\"app\":\"hacBrewPackLZ\",\"version\":" + jsonString(AppVersion) + ",\"platform\":\"Switch\",\"kind\":" + jsonString(kind) +
            ",\"fingerprint\":" + jsonString(fingerprint) + ",\"title\":" + jsonString(title) + ",\"said\":" + jsonString(said) + ",\"report\":" + jsonString(body) +
            ",\"device\":" + jsonString(deviceName()) + ",\"count\":1}";
        std::string noticeTitle, noticeText;
        try {
            auto reply = _platform.post(reportAddress() + "/report", json, [] { return true; });
            std::string text(reply.second.begin(), reply.second.end());
            std::string status = jsonValue(text, "status");
            if (reply.first == 200 && status == "received") {
                noticeTitle = "报告已发送";
                noticeText = "已作为报告发送 " + jsonValue(text, "id") + "，谢谢，报告会直接发送给开发者。";
            } else if (reply.first == 200 && (status == "fixed" || status == "known")) {
                std::string fixedIn = jsonValue(text, "fixedIn");
                noticeTitle = "已知问题";
                noticeText = status == "fixed" && !fixedIn.empty() ? "已在版本 " + fixedIn + " 中修复，更新后即可解决。" : "已知晓，正在处理。";
            } else {
                std::string why = jsonValue(text, "error");
                throw ToolError(why.empty() ? format("报告服务返回 %ld", reply.first) : why);
            }
        } catch (const std::exception& e) {
            logLine(std::string("报告发送失败：") + e.what());
            noticeTitle = "已改为保存报告";
            noticeText = std::string("发送失败（") + e.what() + "），已改为保存为 " + _platform.dataFolder() + "/report.txt";
            try {
                std::string text = said.empty() ? report : "我当时的操作：" + said + "\n\n" + report;
                sys::File file(_platform.dataFolder() + "/report.txt", sys::File::Mode::Create);
                file.write(0, text.data(), text.size());
                file.flush();
            } catch (const ToolError& saving) {
                noticeText = std::string("发送与保存均失败：") + saving.what();
            }
        }
        {
            sys::Lock lock(_mutex);
            _updateNoticeTitle = noticeTitle;
            _updateNoticeText = noticeText;
            _dirty = true;
        }
        _reporting = false;
    }, -1, 1 << 18);
}

std::string App::reportText() {
    std::string text = format("hacBrewPackLZ %s, a report of what happened\n", AppVersion);
    time_t now = time(nullptr);
    char when[64] = {};
    if (struct tm* local = localtime(&now))
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M", local);
    text += format("Made %s\n%s\n", when, _platform.aboutSystem().c_str());

    std::string settings;
    try {
        settings = readSaved("settings.txt");
    } catch (const ToolError&) {
    }
    text += "\n== Settings\n" + settings;

    {
        sys::Lock lock(_mutex);
        static const char* const tabs[TabCount] = {"已安装游戏", "游戏文件"};
        for (int tab = 0; tab < TabCount; tab++) {
            text += format("\n== %s\n", tabs[tab]);
            for (auto& row : _rows) {
                if (row.tab != tab)
                    continue;
                text += format("%s  %s  %s  %s, %s", row.key.c_str(), row.name.c_str(), row.version.c_str(), stateName((int)row.state), formatSize(row.now).c_str());
                if (row.hasEstimate)
                    text += format(", %s after", formatSize(row.estimate.outputBytes).c_str());
                if (!row.ticked)
                    text += ", not ticked";
                text += "\n";
                for (auto* said : {&row.blocked, &row.message, &row.belowMinimum}) {
                    if (!said->empty())
                        text += "    " + *said + "\n";
                }
            }
        }
        if (!_results.empty()) {
            text += "\n== The last job: " + _resultsTitle + " " + _resultsDetail + "\n";
            for (auto& result : _results)
                text += result.name + ": " + result.outcome + (result.message.empty() ? "" : " (" + result.message + ")") + "\n";
        }
    }

    try {
        text += "\n== How opening went\n" + readTextFile(_platform.dataFolder() + "/opening.txt");
    } catch (const ToolError&) {
        text += "(not there)\n";
    }
    std::vector<std::string> secrets = _keyset.secrets();
    text += "\n== What Atmosph\xC3\xA8re says crashed lately\n" + crashReports(_platform.crashReportFolders(), secrets);
    {
        sys::Lock lock(_logMutex);
        text += format("\n== The last %d lines of what the app has said\n", (int)_recentLog.size());
        for (auto& line : _recentLog)
            text += line;
    }
    return withoutKeys(text, secrets);
}

void App::saveReport(const std::string& said) {
    std::string path = _platform.dataFolder() + "/report.txt";
    auto dialog = std::make_unique<Dialog>();
    try {
        std::string text = said.empty() ? reportText() : "我当时的操作：" + said + "\n\n" + reportText();
        {
            sys::File file(path, sys::File::Mode::Create);
            file.write(0, text.data(), text.size());
            file.flush();
        }
        sys::commit(path);
        logLine("报告已保存到 " + path);
        dialog->title = "报告已保存";
        dialog->paragraphs.push_back("It's at " + path + "，包含列表内容、事情经过、程序日志与 Atmosphère 的近期崩溃记录，不含任何密钥。");
        dialog->paragraphs.push_back("请把报告与当时的操作一起作为新 issue 发到 GitLab（需要账号）。二维码可在手机上打开该页面。");
        dialog->qr = NewIssuePage;
        dialog->qrCaption = "New issue";
    } catch (const ToolError& e) {
        dialog->title = "报告保存失败";
        dialog->paragraphs.push_back(e.what());
    }
    dialog->choices = {{"确定", nullptr}};
    dialog->cancelChoice = 0;
    _menu.reset();
    _dialog = std::move(dialog);
    _dirty = true;
}

/// Rebuilt by this tool means generation 0 keys, the only ones it writes, along with either compressed data, which no game that old
/// has, or a version of its own, which only comes from an update made part of it.
bool App::rebuiltHere(const InstalledGame& game, const InstalledMeta& base) {
    if (base.storage == Storage::GameCard || !_console || _keyset.empty())
        return false;
    std::string key = rebuiltKey(fingerprintOf(game));
    {
        sys::Lock lock(_mutex);
        auto found = _cache.find(key);
        if (found != _cache.end())
            return found->second == "1";
    }
    bool rebuilt = false;
    for (auto& content : base.contents) {
        if (content.type != Cnmt::ContentTypeProgram)
            continue;
        try {
            TicketStore none(_keyset);
            NcaFile nca(_console->openContent(base.storage, content.id, content.size), content.id.hex() + ".nca", _keyset, none);
            bool compressed = false;
            for (int i = 0; i < NcaFile::SectionCount; i++)
                compressed = compressed || (nca.section(i) && nca.section(i)->hasCompression());
            rebuilt = nca.generationIndex() == 0 && (compressed || base.key.version > 0);
        } catch (const ToolError& e) {
            // Not remembered, as it may read next time.
            logLine(format("%016llx's program couldn't be read to see if it's been rebuilt: %s", (unsigned long long)game.applicationId, e.what()));
            return false;
        }
        break;
    }
    sys::Lock lock(_mutex);
    _cache[key] = rebuilt ? "1" : "0";
    saveCacheSoonLocked();
    return rebuilt;
}

bool App::updatedOverRebuilt(const InstalledGame& game) {
    const InstalledMeta* base = nullptr;
    bool patched = false;
    for (auto& meta : game.metas) {
        if (meta.key.type == Cnmt::MetaApplication)
            base = &meta;
        else if (meta.key.type == Cnmt::MetaPatch)
            patched = true;
    }
    return base && patched && rebuiltHere(game, *base);
}

/// What a game's record asks of the system, as the console goes by it. Zero if it doesn't say.
static u32 recordedSystem(const Bytes& record) {
    return record.size() >= 8 + 0xC && rd16(record.data()) >= 0xC ? rd32(record.data() + 8 + 8) : 0;
}

/// Two things on a copy's record stop it starting, though the copy is whole. Before version 1.5, a copy rebuilt from a game that's only whole
/// with its update kept saying so, as that game does, so the console asks for an update. And a copy made in place of a game that an installer
/// such as DBI had let start on an older system took the system version from the game's files, so the console asks for a system update: that's
/// what 1.5.0 test 2 and older did. With an update installed, it has what it asks for: an update over a copy is for judgeUpdateOver.
void App::judgeMark(InstalledGame& game) {
    if (!game.blocked.empty())
        return;
    const InstalledMeta* base = nullptr;
    for (auto& meta : game.metas) {
        if (meta.key.type == Cnmt::MetaPatch)
            return;
        if (meta.key.type == Cnmt::MetaApplication)
            base = &meta;
    }
    if (!base)
        return;
    // A copy can only ask for a version the console doesn't have once the console is new enough to read compressed games at all: lowering what
    // it asks for wouldn't make it start before that.
    u32 system = _platform.systemVersion();
    bool compacted = base->attributes & Cnmt::AttributeCompacted;
    if ((!compacted && system < CompressionMinSystemVersion) || !rebuiltHere(game, *base))
        return;
    if (system >= CompressionMinSystemVersion) {
        try {
            u32 asks = recordedSystem(_console->getMeta(base->storage, base->key));
            // By major, minor and micro version, as what follows is the build of a version, which the console doesn't go by.
            if ((asks >> 16) > (system >> 16)) {
                game.asksSystem = asks;
                game.markedIncomplete = true;
            }
        } catch (const ToolError& e) {
            logLine(format("%016llx's record couldn't be read to see what it asks of the system: %s", (unsigned long long)game.applicationId, e.what()));
        }
    }
    if (!compacted)
        return;
    // This tool never writes part of a game, but it's the console's own record that changes, so the copy is looked at to be sure it's whole.
    // Only games that say they need their update get this far, which is a few at most.
    for (auto& content : base->contents) {
        if (content.type == Cnmt::ContentTypeMeta)
            continue;
        try {
            TicketStore none(_keyset);
            NcaFile nca(_console->openContent(base->storage, content.id, content.size), content.id.hex() + ".nca", _keyset, none);
            for (int i = 0; i < NcaFile::SectionCount; i++) {
                if (nca.section(i) && nca.section(i)->hasSparse()) {
                    logLine(format("%016llx says it needs its update, and part of it is left out, so it does", (unsigned long long)game.applicationId));
                    return;
                }
            }
        } catch (const ToolError& e) {
            logLine(format("%016llx says it needs its update, and couldn't be read to see if it's whole: %s", (unsigned long long)game.applicationId, e.what()));
            return;
        }
    }
    game.markedIncomplete = true;
}

/// An update newer than what's in the copy is made part of it by compressing the game, the same as an update's file would be. One that's no
/// newer has nothing to add to it, and only stops it starting until it's taken away.
void App::judgeUpdateOver(InstalledGame& game) {
    if (!game.blocked.empty() || !updatedOverRebuilt(game))
        return;
    u32 copy = 0, update = 0;
    for (auto& meta : game.metas) {
        if (meta.key.type == Cnmt::MetaApplication)
            copy = std::max(copy, meta.key.version);
        else if (meta.key.type == Cnmt::MetaPatch)
            update = std::max(update, meta.key.version);
    }
    if (update > copy)
        game.updateOverCopy = true;
    else
        game.blocked = UpdateNoNewer;
}

/// Found out when the game's analyzed, as that reads it anyway, rather than part way through compressing it. A copy made with an update in it
/// before version 1.5 doesn't say where that update kept the original's parts, which a newer one reads, so that update's own file has to say,
/// and a copy of a game its makers compressed doesn't hold the original as it was stored, which takes the original.
void App::checkUpdateFits(const TitleJob& job, bool installedOverIt) {
    if (!job.update || updateFits(job))
        return;
    bool copy = false, mapped = false, compressedCopy = false;
    for (auto& content : job.contents) {
        if (!content.base)
            continue;
        mapped = mapped || !content.base->keptOriginal.empty();
        if (content.type != Cnmt::ContentTypeProgram)
            continue;
        bool compressed = false;
        for (int i = 0; i < NcaFile::SectionCount; i++)
            compressed = compressed || (content.base->section(i) && content.base->section(i)->hasCompression());
        copy = content.base->generationIndex() == 0 && (compressed || job.main->meta.version > 0);
        compressedCopy = compressed;
    }
    // A game made with compression off, with an update in it, is said as what it is: repacked.
    const char* kind = compressedCopy ? "压缩" : "重打包";
    u32 inIt = job.main->meta.version;
    if (copy && inIt > 0 && !mapped) {
        throw ToolError(installedOverIt
            ? format("此%s游戏已含 v%u 更新补丁，缺少新补丁所需的原始本体内容，因此无法启动。请移除新补丁，并把它的文件连同 v%u 补丁文件或原始游戏一起放到「游戏文件」", kind, inIt, inIt)
            : format("已安装的%s游戏已含 v%u 更新补丁，缺少此补丁所需的原始本体内容。请把 v%u 补丁文件或原始游戏也放到「游戏文件」", kind, inIt, inIt));
    }
    if (copy) {
        throw ToolError(installedOverIt ? format("此更新补丁需要原始本体中的部分内容，而%s游戏没有，因此无法启动。请移除该补丁，并把它的文件连同原始游戏一起放到「游戏文件」", kind)
                                        : format("已安装的%s游戏缺少此更新补丁所需的原始本体内容。请把原始游戏一并放到「游戏文件」", kind));
    }
    throw ToolError("无法把此更新补丁加入已安装游戏：游戏已损坏，或补丁对应的是其他版本");
}

}  // namespace hbp
