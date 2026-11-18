// What the app's own files share: limits and names used in more than one of them, and small helpers for paths and text.
#pragma once

#include <algorithm>
#include <cstring>

#include "app.h"

namespace hbp {
namespace app {

const int MinSavingChoices[] = {0, 5, 10, 15, 20, 25, 30, 40, 50};

/// Space on top of the data itself that's kept free while working.
constexpr i64 RoomToSpare = 64LL << 20;

/// A title that wouldn't shrink by even this much is already as small as it gets.
constexpr double NothingToGain = 0.005;

/// Compressing never makes game data more than a fraction of a percent bigger, so a game that would grow by more is packed with something
/// stronger already: a fact about it. Less is only as small as it gets. As SavingRule::notSure has it.
constexpr double GrowsForSure = 0.01;

/// A game this small with nothing else to it isn't a game, but something like a forwarder.
constexpr i64 SmallGameBytes = 1 << 20;

constexpr int ListIconSize = 60;
constexpr int JobIconSize = 128;

/// How a remembered answer says that there wasn't one, and why.
constexpr const char* RememberedFailure = "failed ";
/// After a game's remembered answer, what says its one file, with its DLC or with the other DLC of a game that isn't there, was found damaged
/// as it was saved.
constexpr const char* OneFileFailure = "|one file";

/// What every remembered answer starts with. It changes when answers from before can't be gone by, because more games can be read than could be
/// then, or because they weren't right, and they all get worked out again.
constexpr const char* AnswerGeneration = "5|";

/// Between the parts of a row's second line.
constexpr const char* Dot = "  \xC2\xB7  ";

// Where things go on the screen, which is always 1280 by 720.
constexpr int ScreenW = 1280;
constexpr int ScreenH = 720;
constexpr int Margin = 40;
constexpr int TopBarH = 72;
/// Tall enough for the views to be a control of their own, rather than one more column heading.
constexpr int ColumnsH = 48;
constexpr int ListTop = TopBarH + ColumnsH;
constexpr int RowH = 74;
constexpr int VisibleRows = 7;
constexpr int ListH = RowH * VisibleRows;
constexpr int FooterY = ListTop + ListH + 4;
constexpr int HintY = 680;
constexpr int PageRows = VisibleRows - 1;

/// Movement on the screen: short, only ever drawn, so whatever's pressed is done at once, and slowing towards its end.
constexpr double FadeTime = 0.14, RiseTime = 0.16, PopTime = 0.12;
inline double easeOut(double t) {
    t = std::min(std::max(t, 0.0), 1.0);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

/// The font for bytes and text inside games, every character as wide as the others (app_mono_font.cpp).
extern const unsigned char InconsolataRegular[];
extern const size_t InconsolataRegularSize;

/// A count as people write it, with a comma between each three figures.
inline std::string withCommas(long long value) {
    std::string digits = std::to_string(value), out;
    for (size_t i = 0; i < digits.size(); i++) {
        if (i > 0 && digits[i - 1] != '-' && (digits.size() - i) % 3 == 0)
            out += ',';
        out += digits[i];
    }
    return out;
}
inline std::string itemsWord(long long items) { return items == 1 ? std::string("1 项") : withCommas(items) + " 项"; }
// Look inside: rows of what's in a folder, lines of text beside them, and on the whole screen, and where a picture has to fit in each.
constexpr int InsideItemH = 46;
constexpr int InsideListTop = 132;
constexpr int InsideListBottom = 618;
constexpr int InsidePreviewLines = 20;
constexpr int InsideFullLines = 24;
constexpr int InsidePictureW = 480, InsidePictureH = 410, InsideFolderPictureH = 200;
constexpr int InsideFullPictureW = 1176, InsideFullPictureH = 510;

template <typename T, size_t N>
int choiceIndex(const T (&choices)[N], T value) {
    for (size_t i = 0; i < N; i++) {
        if (choices[i] == value)
            return (int)i;
    }
    return 0;
}

template <typename T, size_t N>
T nextChoice(const T (&choices)[N], T value, int direction) {
    int index = choiceIndex(choices, value) + direction;
    return choices[std::max(0, std::min(index, (int)N - 1))];
}

/// The colour of the tile a game without an icon gets, always the same one for the same name.
inline Color tileColor(const std::string& name) {
    static const Color palette[] = {{52, 118, 196}, {196, 72, 92}, {64, 158, 112}, {150, 96, 190}, {214, 132, 56}, {46, 150, 168}, {120, 128, 148}};
    u32 hash = 5381;
    for (char c : name)
        hash = hash * 33 + (u8)c;
    return palette[hash % (sizeof(palette) / sizeof(palette[0]))];
}

inline Bytes readBytesFile(const std::string& path) {
    sys::File file(path, sys::File::Mode::Read);
    Bytes data((size_t)file.size());
    if (!data.empty())
        file.read(0, data.data(), data.size());
    return data;
}

/// Where reports go, with "none" from a test meaning nowhere, and what's sent and comes back, which is only ever a little JSON.
std::string reportAddress();
std::string jsonString(const std::string& text);
std::string jsonValue(const std::string& json, const char* name);
/// The words a problem is known by, without names, paths or numbers other than error codes, as the PC versions have it.
std::string normalizedProblem(const std::string& message);

inline std::string readTextFile(const std::string& path) {
    Bytes data = readBytesFile(path);
    return std::string(data.begin(), data.end());
}

template <typename Fn>
void forEachLine(const std::string& text, Fn fn) {
    size_t position = 0;
    while (position < text.size()) {
        size_t end = text.find('\n', position);
        if (end == std::string::npos)
            end = text.size();
        fn(text.substr(position, end - position));
        position = end + 1;
    }
}

/// A system version as a game's record writes it, the way people know it: 0x5C000000 is 23.0.0.
inline std::string versionName(u32 version) {
    return format("%u.%u.%u", version >> 26, (version >> 20) & 0x3F, (version >> 16) & 0xF);
}

/// The parts that aren't empty, with the separator between them.
inline std::string joined(const std::vector<std::string>& parts, const char* separator) {
    std::string text;
    for (auto& part : parts) {
        if (part.empty())
            continue;
        if (!text.empty())
            text += separator;
        text += part;
    }
    return text;
}

/// The first few names, and how many more there are.
inline std::string someNames(const std::vector<std::string>& names, size_t shown) {
    std::string text = joined(std::vector<std::string>(names.begin(), names.begin() + (long)std::min(names.size(), shown)), ", ");
    if (names.size() > shown)
        text += format(" 等 %d 项", (int)(names.size() - shown));
    return text;
}

inline std::string fileNameOf(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

inline std::string withoutExtension(const std::string& name) {
    size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

inline std::string joinPath(const std::string& folder, const std::string& name) { return folder + (!folder.empty() && folder.back() == '/' ? "" : "/") + name; }

/// A game that couldn't be read for want of a key or a ticket, which another set of keys, or a ticket installed since, can change.
inline bool wantsKeys(const std::string& failure) {
    return failure.find("not in your keys") != std::string::npos || failure.find("needs a title key") != std::string::npos ||
           failure.find("personalized ticket") != std::string::npos;
}

/// What's worth trying next, after a failure people can do something about. Just the failure, otherwise.
std::string withNextStep(const std::string& failure);
/// What went wrong in a few plain words, for the list, where a failure as it's said, with its files, offsets and codes, doesn't tell people
/// much. The failure itself is under About this game. The same sentences as the PC's (Help.PlainReason).
std::string plainReason(const std::string& failure);

/// A few lines on what people most often need to know, each with where to read the rest.
struct HelpTopic {
    const char* title;
    const char* text;
    const char* anchor;
};
extern const HelpTopic HelpTopics[];
extern const size_t HelpTopicCount;
constexpr const char* HelpPage = "https://gitlab.com/ismaellozanoperezserrano/hacbrewpacklz/-/blob/main/docs/help.md";
constexpr const char* NewIssuePage = "https://gitlab.com/ismaellozanoperezserrano/hacbrewpacklz/-/issues/new";

/// Said of a game this tool rebuilt that has had a newer official update installed over it since, which compressing makes part of it: on the
/// list, and in full in About this game.
constexpr const char* UpdateToMakePart = "整合更新补丁前无法启动";
constexpr const char* UpdateToMakePartAbout = "官方更新补丁覆盖安装到了这个压缩或重打包的游戏上。补丁只适配原始游戏，因此当前无法启动。压缩会把补丁整合进去，用一个压缩游戏替代两者，无需原始游戏文件。";
/// An update that's no newer than the game it went in over has nothing to add to it.
constexpr const char* UpdateNoNewer = "覆盖安装到此游戏的更新补丁并不比已有版本新，却导致无法启动。请用安装游戏的工具移除该补丁";
constexpr const char* UpdateNoNewerShort = "移除更新补丁前无法启动";

inline bool startsWithNoCase(const std::string& text, const std::string& start) { return text.size() >= start.size() && equalsNoCase(text.substr(0, start.size()), start); }

inline bool isWithin(const std::string& path, const std::string& folder) {
    if (folder.empty())
        return false;
    return equalsNoCase(path, folder) || startsWithNoCase(path, folder.back() == '/' ? folder : folder + "/");
}

inline bool isWithinAny(const std::string& path, const std::vector<std::string>& folders) {
    return std::any_of(folders.begin(), folders.end(), [&](const std::string& folder) { return isWithin(path, folder); });
}

inline bool lessNoCase(const std::string& a, const std::string& b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) { return tolower((u8)x) < tolower((u8)y); });
}

/// What this tool made from a game says so in its name.
inline bool isOwnOutput(const std::string& path) { return containsNoCase(fileNameOf(path), "[LZ4]"); }

/// When this was built, which the log starts with (app_built.cpp, compiled afresh for every build).
const char* builtAt();

}  // namespace app
}  // namespace hbp
