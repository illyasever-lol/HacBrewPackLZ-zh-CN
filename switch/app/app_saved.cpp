// What's kept between runs: the settings, what's been worked out about each game, which games are hidden, and the lists as they were
// last time. Everything is written so that a power cut at any moment leaves a whole copy of it somewhere.
#include "app_internal.h"

#include <cstdio>

namespace hbp {

using namespace app;

namespace {

constexpr u32 ListMagic = 0x4C504248;  // "HBPL"
constexpr u32 ListVersion = 3;
constexpr u32 IndexMagic = 0x46504248;  // "HBPF"
/// 2 has each title's display version, so what's known from before gets read again.
constexpr u32 IndexVersion = 2;

/// Whatever's saved ends with this, so anything that doesn't was cut off.
constexpr const char* LastLine = "end\n";

/// What every remembered answer starts with. It changes when answers from before can't be gone by, because more games can be read than could be
/// then, or because they weren't right, and they all get worked out again.
constexpr const char* AnswerGeneration = "4|";
/// Answers from the one before are still good, except where it said a game wasn't compressed and had nothing to gain: some were
/// compressed all along, with a part too small to matter or nothing compressing touches, which it couldn't tell then.
constexpr const char* PreviousGeneration = "3|";

/// The Switch can't reliably open paths containing non-ASCII bytes. Localized
/// builds once wrote Chinese output folder names into settings.txt, so ignore
/// any saved output folder that isn't plain ASCII and use the default instead.
bool asciiPathOnly(const std::string& path) {
    for (unsigned char c : path)
        if (c >= 0x80)
            return false;
    return true;
}

/// What was last saved under a name, which may be waiting beside it if the power went at just the wrong moment. Empty if there's nothing.
std::string savedPath(const std::string& path) {
    if (sys::exists(path))
        return path;
    std::string temp = path + ".new";
    return sys::exists(temp) ? temp : std::string();
}

void checkWritten(const std::string& path, size_t size) {
    sys::File written(path, sys::File::Mode::Read);
    if (written.size() != (i64)size)
        throw ToolError(format("%s came to %lld bytes rather than %lld", path.c_str(), (long long)written.size(), (long long)size));
}

void writeWholeFile(const std::string& path, const void* data, size_t size) {
    auto writeTo = [&](const std::string& target) {
        sys::File file(target, sys::File::Mode::Create);
        if (size > 0)
            file.write(0, data, size);
        file.flush();
    };
    // Written beside the old one, so a power cut can't leave half a file.
    std::string temp = path + ".new";
    writeTo(temp);
    sys::commit(temp);
    sys::removeFile(path);
    try {
        sys::renameFile(temp, path);
    } catch (const ToolError&) {
        // Whatever got in the way of the name, it's no reason to lose what's being saved.
        writeTo(path);
        sys::removeFile(temp);
    }
    sys::commit(path);
    // Better found out now than the next time this opens.
    checkWritten(path, size);
}

/// Over whatever's there, for what's kept in two places, so that there's always a whole copy of it somewhere.
void writeInPlace(const std::string& path, const std::string& text) {
    {
        sys::File file(path, sys::File::Mode::Create);
        file.write(0, text.data(), text.size());
        file.flush();
    }
    sys::commit(path);
    checkWritten(path, text.size());
}

bool endsProperly(const std::string& text) {
    size_t size = strlen(LastLine);
    return text.size() >= size && text.compare(text.size() - size, size, LastLine) == 0;
}

class ListWriter {
public:
    void number(u32 value) {
        size_t at = data.size();
        data.resize(at + 4);
        wr32(&data[at], value);
    }
    void big(i64 value) {
        size_t at = data.size();
        data.resize(at + 8);
        wr64(&data[at], (u64)value);
    }
    void text(const std::string& value) {
        number((u32)value.size());
        data.insert(data.end(), value.begin(), value.end());
    }
    void icon(const std::shared_ptr<Image>& image) {
        bool has = image && image->w == ListIconSize && image->h == ListIconSize;
        number(has ? 1 : 0);
        if (has) {
            const u8* bytes = (const u8*)image->pixels.data();
            data.insert(data.end(), bytes, bytes + image->pixels.size() * sizeof(u32));
        }
    }
    Bytes data;
};

class ListReader {
public:
    explicit ListReader(const Bytes& data) : _data(data) {}
    u32 number() {
        need(4);
        u32 value = rd32(&_data[_at]);
        _at += 4;
        return value;
    }
    i64 big() {
        need(8);
        i64 value = rdi64(&_data[_at]);
        _at += 8;
        return value;
    }
    std::string text() {
        size_t size = number();
        need(size);
        std::string value((const char*)_data.data() + _at, size);
        _at += size;
        return value;
    }
    std::shared_ptr<Image> icon() {
        if (number() == 0)
            return nullptr;
        auto image = std::make_shared<Image>();
        image->w = image->h = ListIconSize;
        image->pixels.resize((size_t)ListIconSize * ListIconSize);
        size_t size = image->pixels.size() * sizeof(u32);
        need(size);
        memcpy(image->pixels.data(), _data.data() + _at, size);
        _at += size;
        return image;
    }

private:
    void need(size_t size) {
        if (size > _data.size() - _at)
            throw ToolError("保存的列表被截断");
    }
    const Bytes& _data;
    size_t _at = 0;
};

}  // namespace

// -------------------------------------------------------------------------------------------------------------------
// Kept twice over

/// Whichever copy is whole. A file from before there were two of them doesn't say whether it is, but it's all there is. The reason's kept for the log,
/// for when something that was saved isn't there.
std::string App::readSaved(const char* name) {
    const std::string paths[] = {_platform.dataFolder() + "/" + name, _platform.backupFolder() + "/" + name, _platform.dataFolder() + "/" + name + ".new"};
    std::string whyNot;
    std::string anything, anythingFrom;
    for (auto& path : paths) {
        std::string text;
        try {
            text = readTextFile(path);
        } catch (const ToolError& e) {
            if (whyNot.empty())
                whyNot = e.what();
            continue;
        }
        if (endsProperly(text)) {
            logLine(format("%s read from %s, %d bytes", name, path.c_str(), (int)text.size()));
            return text;
        }
        if (anythingFrom.empty() && !text.empty()) {
            anything = std::move(text);
            anythingFrom = path;
        }
    }
    if (!anythingFrom.empty()) {
        logLine(format("%s read from %s, %d bytes, with no end to it", name, anythingFrom.c_str(), (int)anything.size()));
        return anything;
    }
    logLine(format("No %s saved yet: %s", name, whyNot.c_str()));
    return "";
}

/// One copy and then the other, so that whenever the power goes, one of them is whole. It's only a problem if neither can be saved.
void App::saveTwice(const char* name, const char* what, const std::string& text) {
    std::string whole = text + LastLine;
    std::string problem;
    int saved = 0;
    for (const std::string& folder : {_platform.backupFolder(), _platform.dataFolder()}) {
        std::string path = folder + "/" + name;
        // The card can be busy for a moment, and the folder may have gone. Neither is worth troubling anyone with.
        constexpr int Tries = 3;
        for (int attempt = 1; attempt <= Tries; attempt++) {
            try {
                writeInPlace(path, whole);
                saved++;
                break;
            } catch (const ToolError& e) {
                problem = e.what();
                logLine(format("Try %d of %d: %s", attempt, Tries, e.what()));
                sys::makeDirectories(folder);
                sys::sleepMs(50 * attempt);
            }
        }
    }
    if (saved == 0)
        noteSaveProblem(what, problem);
    else
        logLine(format("%s saved, %d bytes", name, (int)whole.size()));
}

/// Said on the screen, once, as it's the sort of thing nobody would otherwise find out about until it's all been lost.
void App::noteSaveProblem(const std::string& what, const std::string& why) {
    logLine("无法保存" + what + ": " + why);
    sys::Lock lock(_logMutex);
    if (_saveProblem.empty() && !_saveProblemShown)
        _saveProblem = "无法保存" + what + ". " + why;
}

// -------------------------------------------------------------------------------------------------------------------
// Settings

const std::pair<const char*, bool App::Settings::*> App::SavedFlags[] = {
    {"moresettings", &Settings::moreSettings},
    {"writelog", &Settings::writeLog},
    {"installto", &Settings::installToConsole},
    {"fromsd", &Settings::fromSd},
    {"fromusb", &Settings::fromUsb},
    {"frompcnetwork", &Settings::fromPcNetwork},
    {"frompccable", &Settings::fromPcCable},
    {"hidesmall", &Settings::hideSmall},
    {"hidecompressed", &Settings::hideCompressed},
    {"compress", &Settings::compress},
    {"replace", &Settings::replaceInstalled},
    // A new name, as the old one was saved off when off was all there was to begin with.
    {"installgamefiles", &Settings::installFiles},
    {"deleteinstalled", &Settings::deleteInstalled},
    {"boost", &Settings::boost},
    {"warned", &Settings::warned},
    {"updates", &Settings::autoUpdate},
    {"checkwritten", &Settings::checkResult},
    {"welcomed", &Settings::welcomed},
};

const std::pair<const char*, std::string App::Settings::*> App::SavedFolders[] = {
    {"nspfolder", &Settings::nspFolder},
    {"extractfolder", &Settings::extractFolder},
    {"filesfolder", &Settings::filesFolder},
};

void App::loadSettings() {
    forEachLine(readSaved("settings.txt"), [this](const std::string& line) {
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            return;
        std::string name = line.substr(0, eq);
        std::string word = line.substr(eq + 1);
        long long value = atoll(word.c_str());
        for (auto& flag : SavedFlags) {
            if (name == flag.first)
                _settings.*flag.second = value != 0;
        }
        for (auto& folder : SavedFolders) {
            if (name == folder.first)
                _settings.*folder.second = word;
        }
        if (name == "minimumsaving")
            _settings.minSaving = MinSavingChoices[choiceIndex(MinSavingChoices, (int)value)];
        else if (name == "level")
            _settings.level = std::max(1, std::min(12, (int)value));
        else if (name == "blocksize")
            _settings.blockSize = BlockSizeChoices[choiceIndex(BlockSizeChoices, (int)value)];
        else if (name == "sort")
            _settings.sort = std::max(0, std::min(2, (int)value));
    });
}

void App::saveSettings() {
    std::string text;
    for (auto& flag : SavedFlags)
        text += format("%s=%d\n", flag.first, _settings.*flag.second ? 1 : 0);
    text += format("minimumsaving=%d\nlevel=%d\nblocksize=%d\nsort=%d\n", _settings.minSaving, _settings.level, _settings.blockSize, _settings.sort);
    for (auto& folder : SavedFolders)
        text += std::string(folder.first) + "=" + _settings.*folder.second + "\n";
    saveTwice("settings.txt", "设置", text);
}

ConvertOptions App::convertOptions() const {
    ConvertOptions options;
    options.compress = _settings.compress;
    options.level = _settings.level;
    options.blockSize = _settings.blockSize;
    options.minSavingPercent = _settings.minSaving;
    options.threads = sys::workerCount();
    options.tempDir = _platform.dataFolder() + "/temp";
    options.holdInMemory = sys::spareMemory();
    options.keepSmallSections = std::min<i64>(options.holdInMemory, 256LL << 20);
    options.checkResult = _settings.checkResult;
    return options;
}

std::string App::nspFolder() const {
    return _settings.nspFolder.empty() || !asciiPathOnly(_settings.nspFolder) ? _platform.outputFolder() : _settings.nspFolder;
}

std::string App::extractFolder() const {
    return _settings.extractFolder.empty() || !asciiPathOnly(_settings.extractFolder) ? joinPath(_platform.outputFolder(), "extracted") : _settings.extractFolder;
}

// -------------------------------------------------------------------------------------------------------------------
// What's been worked out about each game

std::string App::cacheKey(const Row& row) const {
    return AnswerGeneration + (_settings.compress ? format("%s|%s|%d|%d", row.key.c_str(), row.fingerprint.c_str(), _settings.level, _settings.blockSize) : format("%s|%s|0|0", row.key.c_str(), row.fingerprint.c_str()));
}

void App::loadCache() {
    forEachLine(readSaved("analysis.txt"), [this](const std::string& line) {
        size_t tab = line.find('\t');
        if (tab == std::string::npos)
            return;
        if (line.rfind(AnswerGeneration, 0) == 0) {
            _cache[line.substr(0, tab)] = line.substr(tab + 1);
            return;
        }
        if (line.rfind(PreviousGeneration, 0) != 0)
            return;
        long long in = 0, out = 0, margin = 0, romFs = 0, stored = 0;
        int compressed = 0;
        bool compressing = !endsWithNoCase(line.substr(0, tab), "|0|0");
        bool askAgain = compressing && sscanf(line.c_str() + tab + 1, "%lld %lld %lld %lld %lld %d", &in, &out, &margin, &romFs, &stored, &compressed) == 6 && !compressed && out >= in;
        if (!askAgain)
            _cache[AnswerGeneration + line.substr(strlen(PreviousGeneration), tab - strlen(PreviousGeneration))] = line.substr(tab + 1);
        _cacheChanged = true;
    });
}

void App::saveCacheLocked() {
    std::string text;
    for (auto& entry : _cache)
        text += entry.first + "\t" + entry.second + "\n";
    _cacheChanged = false;
    _cacheSavedAt = sys::now();
    saveTwice("analysis.txt", "已得出的结果", text);
}

/// With hundreds of games to get through, not after every one of them, and often enough that a power cut wouldn't lose much.
void App::saveCacheSoonLocked() {
    constexpr double SaveEverySeconds = 5;
    _cacheChanged = true;
    if (sys::now() - _cacheSavedAt >= SaveEverySeconds)
        saveCacheLocked();
}

void App::rememberEstimateLocked(const Row& row) {
    const SizeEstimate& e = row.estimate;
    _cache[cacheKey(row)] = format("%lld %lld %lld %lld %lld %d", (long long)e.inputBytes, (long long)e.outputBytes, (long long)e.marginBytes, (long long)e.romFsBytes, (long long)e.romFsStoredBytes, e.alreadyCompressed ? 1 : 0);
    saveCacheSoonLocked();
}

/// A failure that would only go the same way again, every time this opened, until it's asked to.
void App::rememberFailureLocked(const Row& row, const std::string& failure) {
    _cache[cacheKey(row)] = RememberedFailure + failure;
    saveCacheSoonLocked();
}

bool App::recallEstimateLocked(Row& row) {
    auto remembered = _cache.find(cacheKey(row));
    if (remembered == _cache.end())
        return false;
    if (remembered->second.rfind(RememberedFailure, 0) == 0) {
        row.state = RowState::Failed;
        row.message = remembered->second.substr(strlen(RememberedFailure));
        return false;
    }
    long long input, output, margin, romfs, stored;
    int compressed;
    if (sscanf(remembered->second.c_str(), "%lld %lld %lld %lld %lld %d", &input, &output, &margin, &romfs, &stored, &compressed) != 6)
        return false;
    row.hasEstimate = true;
    row.estimate = SizeEstimate{input, output, margin, romfs, stored, compressed != 0};
    return true;
}

// -------------------------------------------------------------------------------------------------------------------
// Hidden games

void App::loadHidden() {
    forEachLine(readSaved("hidden.txt"), [this](const std::string& line) {
        if (!line.empty() && line + "\n" != LastLine)
            _hidden.insert(line);
    });
}

void App::saveHiddenLocked() {
    std::string text;
    for (auto& key : _hidden)
        text += key + "\n";
    saveTwice("hidden.txt", "哪些游戏被隐藏", text);
}

// -------------------------------------------------------------------------------------------------------------------
// The lists as they were

/// The installed games as they were last time, up on the screen straight away. They get gone over afterwards, but finding what a game is
/// called and what it looks like is what takes the time, and that only changes when the game does.
void App::loadList() {
    std::string path = savedPath(_platform.dataFolder() + "/list.cache");
    if (path.empty())
        return;
    try {
        Bytes data = readBytesFile(path);
        ListReader reader(data);
        if (reader.number() != ListMagic || reader.number() != ListVersion)
            return;
        ConvertOptions options = convertOptions();
        u32 count = reader.number();
        sys::Lock lock(_mutex);
        for (u32 i = 0; i < count; i++) {
            Row row;
            row.tab = (int)reader.number();
            row.key = reader.text();
            row.application = (u64)reader.big();
            row.name = reader.text();
            row.version = reader.text();
            row.detail = reader.text();
            row.fingerprint = reader.text();
            row.now = reader.big();
            row.storage = (Storage)reader.number();
            row.extracted = reader.text();
            row.tiny = reader.number() != 0;
            row.hasUpdate = reader.number() != 0;
            row.icon = reader.icon();
            row.tile = tileColor(row.name);
            if (row.tab != TabInstalled || !_console || _hidden.count(row.key))
                continue;
            if (recallEstimateLocked(row)) {
                judgeLocked(row, options);
                _sortedByGain[row.tab] = true;
            } else if (row.state == RowState::Failed) {
                row.ticked = false;
            }
            _rows.push_back(std::move(row));
        }
        sortRowsLocked();
    } catch (const ToolError& e) {
        logLine(std::string("无法读取保存的列表：") + e.what());
        sys::Lock lock(_mutex);
        _rows.clear();
    }
}

void App::saveList() {
    sys::Lock saving(_listSaving);
    ListWriter writer;
    {
        sys::Lock lock(_mutex);
        if (!_listChanged)
            return;
        _listChanged = false;
        writer.number(ListMagic);
        writer.number(ListVersion);
        u32 count = 0;
        for (auto& row : _rows)
            count += row.tab == TabInstalled ? 1 : 0;
        writer.number(count);
        for (auto& row : _rows) {
            if (row.tab != TabInstalled)
                continue;
            writer.number((u32)row.tab);
            writer.text(row.key);
            writer.big((i64)row.application);
            writer.text(row.name);
            writer.text(row.version);
            writer.text(row.detail);
            writer.text(row.fingerprint);
            writer.big(row.now);
            writer.number((u32)row.storage);
            writer.text(row.extracted);
            writer.number(row.tiny ? 1 : 0);
            writer.number(row.hasUpdate ? 1 : 0);
            writer.icon(row.icon);
        }
    }
    try {
        writeWholeFile(_platform.dataFolder() + "/list.cache", writer.data.data(), writer.data.size());
    } catch (const ToolError& e) {
        noteSaveProblem("游戏列表", e.what());
    }
}

/// What's known about the game files, so that those that haven't changed never have to be opened just to find out what they are. It's the same idea
/// as Everything on Windows, which answers from what it has already noted down instead of going to the disk. There's no reading the card's own tables
/// from here, but going through the folders only takes a moment, and it's opening every file that takes the time.
void App::loadFileIndex() {
    std::string path = savedPath(_platform.dataFolder() + "/files.cache");
    if (path.empty())
        return;
    try {
        Bytes data = readBytesFile(path);
        ListReader reader(data);
        if (reader.number() != IndexMagic || reader.number() != IndexVersion)
            return;
        u32 count = reader.number();
        sys::Lock lock(_mutex);
        for (u32 i = 0; i < count; i++) {
            std::string file = reader.text();
            IndexedFile entry;
            entry.size = reader.big();
            entry.extracted = reader.number() != 0;
            entry.problem = reader.text();
            u32 titles = reader.number();
            for (u32 t = 0; t < titles; t++) {
                IndexedTitle title;
                title.id = (u64)reader.big();
                title.type = (u8)reader.number();
                title.version = reader.number();
                title.linked = (u64)reader.big();
                title.bytes = reader.big();
                title.holdsOnlyPart = reader.number() != 0;
                title.name = reader.text();
                title.displayVersion = reader.text();
                title.icon = reader.icon();
                entry.titles.push_back(std::move(title));
            }
            _fileIndex.emplace(std::move(file), std::move(entry));
        }
    } catch (const ToolError& e) {
        logLine(std::string("无法读取游戏文件的已知信息：") + e.what());
        sys::Lock lock(_mutex);
        _fileIndex.clear();
    }
    sys::Lock lock(_mutex);
    showIndexRowsLocked(indexedPathsLocked(fileRoots()), false);
    logLine(format("Known about %d game files and folders", (int)_fileIndex.size()));
}

void App::saveFileIndex() {
    sys::Lock saving(_indexSaving);
    ListWriter writer;
    {
        sys::Lock lock(_mutex);
        if (!_fileIndexChanged)
            return;
        _fileIndexChanged = false;
        writer.number(IndexMagic);
        writer.number(IndexVersion);
        writer.number((u32)_fileIndex.size());
        for (auto& item : _fileIndex) {
            writer.text(item.first);
            writer.big(item.second.size);
            writer.number(item.second.extracted ? 1 : 0);
            writer.text(item.second.problem);
            writer.number((u32)item.second.titles.size());
            for (auto& title : item.second.titles) {
                writer.big((i64)title.id);
                writer.number(title.type);
                writer.number(title.version);
                writer.big((i64)title.linked);
                writer.big(title.bytes);
                writer.number(title.holdsOnlyPart ? 1 : 0);
                writer.text(title.name);
                writer.text(title.displayVersion);
                writer.icon(title.icon);
            }
        }
    }
    try {
        writeWholeFile(_platform.dataFolder() + "/files.cache", writer.data.data(), writer.data.size());
    } catch (const ToolError& e) {
        noteSaveProblem("游戏文件的已知信息", e.what());
    }
}

}  // namespace hbp
