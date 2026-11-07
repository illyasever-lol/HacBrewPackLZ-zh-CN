// What the app's own files share: limits and names used in more than one of them, and small helpers for paths and text.
#pragma once

#include <algorithm>
#include <cstring>

#include "app.h"

namespace hbp {
namespace app {

const int MinSavingChoices[] = {0, 5, 10, 15, 20, 25, 30, 40, 50};
const int BlockSizeChoices[] = {0x1000, 0x2000, 0x4000, 0x8000, 0x10000};

/// Space on top of the data itself that's kept free while working.
constexpr i64 RoomToSpare = 64LL << 20;

/// A title that wouldn't shrink by even this much is already as small as it gets.
constexpr double NothingToGain = 0.005;

/// A game this small with nothing else to it isn't a game, but something like a forwarder.
constexpr i64 SmallGameBytes = 1 << 20;

constexpr int ListIconSize = 60;
constexpr int JobIconSize = 128;

/// How a remembered answer says that there wasn't one, and why.
constexpr const char* RememberedFailure = "failed ";

/// Between the parts of a row's second line.
constexpr const char* Dot = "  \xC2\xB7  ";

// Where things go on the screen, which is always 1280 by 720.
constexpr int ScreenW = 1280;
constexpr int ScreenH = 720;
constexpr int Margin = 40;
constexpr int TopBarH = 72;
constexpr int ColumnsH = 32;
constexpr int ListTop = TopBarH + ColumnsH;
constexpr int RowH = 76;
constexpr int VisibleRows = 7;
constexpr int ListH = RowH * VisibleRows;
constexpr int FooterY = ListTop + ListH + 4;
constexpr int HintY = 680;
constexpr int PageRows = VisibleRows - 1;

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
        text += format(" and %d more", (int)(names.size() - shown));
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

}  // namespace app
}  // namespace hbp
