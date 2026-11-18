// What's kept between runs: the settings, what's been worked out about each game, which games are hidden, and the lists as they were
// last time. Everything is written so that a power cut at any moment leaves a whole copy of it somewhere.
#include "app_internal.h"

#include <cstdio>

#include "../core/remote.h"

namespace hbp {

using namespace app;

namespace {

constexpr u32 ListMagic = 0x4C504248;  // "HBPL"
constexpr u32 ListVersion = 3;
constexpr u32 IndexMagic = 0x46504248;  // "HBPF"
/// 2 has each title's display version, 3 whether this tool made it, by what's in it rather than its name, and what it came to before, and 4
/// its largest stream packed solid, so what's known from before gets read again.
constexpr u32 IndexVersion = 4;

/// Whatever's saved ends with this, so anything that doesn't was cut off.
constexpr const char* LastLine = "end\n";

/// Answers from the one before, 1.4.3's, are still good where nothing that came since changes them: a piece of the original game that an update
/// holds more than once is stored once now, and game data under 1 MB is left as it is. Which those are is only known once their game comes up
/// (App::stillGoodFromBefore).
constexpr const char* PreviousGeneration = "4|";

/// Less game data than this is left as it is now (MinGameDataToCompress in core/title.cpp).
constexpr long long LeftAsItIs = 1 << 20;

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
int App::saveTwice(const char* name, const char* what, const std::string& text) {
    std::string whole = text + LastLine;
    std::string problem;
    int saved = 0;
    for (const std::string& folder : {_platform.backupFolder(), _platform.dataFolder()}) {
        std::string path = folder + "/" + name;
        // The card can be busy for a moment, and the folder may have gone. Neither is worth troubling anyone with. The system turning a file
        // down (2001-0106) is asked about once each time (askCard in nx_sys.cpp), as each time is a chance of the console going down.
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
    return saved;
}

/// Said on the screen, once, as it's the sort of thing nobody would otherwise find out about until it's all been lost.
void App::noteSaveProblem(const std::string& what, const std::string& why) {
    logLine("无法保存 " + what + ": " + why);
    sys::Lock lock(_logMutex);
    if (_saveProblem.empty() && !_saveProblemShown)
        _saveProblem = "无法保存 " + what + ". " + why;
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
    {"pcwififirst", &Settings::wifiFirst},
    {"fromgamecard", &Settings::fromGameCard},
    {"hidesmall", &Settings::hideSmall},
    {"hidecompressed", &Settings::hideCompressed},
    {"compress", &Settings::compress},
    {"replace", &Settings::replaceInstalled},
    // A new name, as the old one was saved off when off was all there was to begin with.
    {"installgamefiles", &Settings::installFiles},
    {"dlcingamefile", &Settings::dlcInGameFile},
    {"deleteinstalled", &Settings::deleteInstalled},
    {"boost", &Settings::boost},
    {"warned", &Settings::warned},
    {"updates", &Settings::autoUpdate},
    {"sendproblems", &Settings::sendProblems},
    {"problemsasked", &Settings::problemsAsked},
    // A new name, as test builds saved it off when off was all there was to begin with.
    {"measureall", &Settings::thorough},
    {"deleteasked", &Settings::deleteAsked},
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
        else if (name == "sort")
            _settings.sort = std::max(0, std::min(2, (int)value));
        else if (name == "viewinstalled" || name == "viewfiles")
            _settings.view[name == "viewfiles" ? 1 : 0] = std::max(0, std::min(2, (int)value));
    });
    remote::setWifiFirst(_settings.wifiFirst);
}

void App::saveSettings() {
    std::string text;
    for (auto& flag : SavedFlags)
        text += format("%s=%d\n", flag.first, _settings.*flag.second ? 1 : 0);
    text += format("minimumsaving=%d\nlevel=%d\nsort=%d\n", _settings.minSaving, _settings.level, _settings.sort);
    if (_settings.view[0] >= 0)
        text += format("viewinstalled=%d\n", _settings.view[0]);
    if (_settings.view[1] >= 0)
        text += format("viewfiles=%d\n", _settings.view[1]);
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
    options.dlcInGameFile = _settings.dlcInGameFile;
    options.threads = sys::workerCount();
    options.tempDir = _platform.dataFolder() + "/temp";
    // A new copy is always read back from the card, which works out its hash and checks every byte of it in the one read. Kept in memory
    // instead, its pieces broke the rest of memory up into bits too small to use, and games ran out of it with a gigabyte free.
    options.holdInMemory = 0;
    options.keepSmallSections = std::min<i64>(sys::spareMemory(), 256LL << 20);
    return options;
}

std::string App::nspFolder() const { return _settings.nspFolder.empty() ? _platform.outputFolder() : _settings.nspFolder; }

std::string App::extractFolder() const { return _settings.extractFolder.empty() ? joinPath(_platform.outputFolder(), "extracted") : _settings.extractFolder; }

std::string App::filesFromGamesFolder() const { return joinPath(_platform.outputFolder(), "Files from games"); }

// -------------------------------------------------------------------------------------------------------------------
// What's been worked out about each game

std::string App::cacheKey(const Row& row) const {
    return AnswerGeneration + (_settings.compress ? format("%s|%s|%d|%d", row.key.c_str(), row.fingerprint.c_str(), _settings.level, _settings.blockSize) : format("%s|%s|0|0", row.key.c_str(), row.fingerprint.c_str()));
}

std::string App::rebuiltKey(const std::string& fingerprint) const { return AnswerGeneration + std::string("rebuilt|") + fingerprint; }

/// What's put right here is put right the same way each time this opens, so it waits to be saved with something that has really changed. Saved
/// for that alone, both copies were written again every time the app was opened and closed, while an answer from the one before waited for its
/// game to come up.
void App::loadCache() {
    std::string saved = readSaved("analysis.txt");
    // What both copies hold, when they hold the same, is what a save that comes out the same can leave as it is. Reading it doesn't wear the
    // card; writing it again does.
    _cacheOnCard.clear();
    try {
        if (endsProperly(saved) && readTextFile(_platform.dataFolder() + "/analysis.txt") == saved && readTextFile(_platform.backupFolder() + "/analysis.txt") == saved)
            _cacheOnCard = saved;
    } catch (const ToolError&) {
    }
    forEachLine(saved, [this](const std::string& line) {
        size_t tab = line.find('\t');
        if (tab == std::string::npos)
            return;
        std::string key = line.substr(0, tab), answer = line.substr(tab + 1);
        bool current = key.rfind(AnswerGeneration, 0) == 0;
        if (!current) {
            // Anything older gets worked out again.
            if (key.rfind(PreviousGeneration, 0) != 0)
                return;
            key = AnswerGeneration + key.substr(strlen(PreviousGeneration));
        }
        bool failure = answer.rfind(RememberedFailure, 0) == 0;
        // Remembered by an earlier version, a game that wanted keys gets another go, as they may have been dumped again since.
        if (failure && wantsKeys(answer))
            return;
        // What the one before worked out with compression on waits for its game to come up, which says whether it's still good. With
        // compression off nothing has changed, and whether a game was rebuilt is about what's installed.
        if (!current && !failure && key.rfind(rebuiltKey(""), 0) != 0 && !endsWithNoCase(key, "|0|0")) {
            _previousAnswers[key] = answer;
            return;
        }
        _cache[key] = answer;
    });
    // Worked out again since, by a version that kept the one before's beside it.
    for (auto& entry : _cache)
        _previousAnswers.erase(entry.first);
}

void App::saveCacheLocked() {
    std::string text;
    for (auto& entry : _cache)
        text += entry.first + "\t" + entry.second + "\n";
    // Those for games that haven't come up yet, such as on a USB drive that isn't in, stay as they were until they do.
    for (auto& entry : _previousAnswers)
        text += PreviousGeneration + entry.first.substr(strlen(AnswerGeneration)) + "\t" + entry.second + "\n";
    _cacheChanged = false;
    // A game worked out again usually comes to what it did before, and both copies already say so.
    if (text + LastLine == _cacheOnCard) {
        logLine("analysis.txt not saved: it would come out the same as on the card");
        return;
    }
    _cacheSavedAt = sys::now();
    _cacheOnCard = saveTwice("analysis.txt", "what's been worked out", text) == 2 ? text + LastLine : std::string();
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
    // What came after the first six goes last, where a version from before it leaves it alone: whether it was measured, what compressing it
    // freed, whether its makers compressed it, and whether it's a copy an older version made.
    // A packed file is kept as the file itself, and what it takes installed as it is with it, whichever it's being compared with now.
    i64 input = e.packedBytes > 0 ? e.packedBytes : e.inputBytes;
    // The twelfth field says this version's way of telling the copies this tool made worked it out, as one that took older ones for its
    // makers' didn't, and they're looked at once more. The thirteenth says it was worked out as games are now: one under 1 GB measured
    // exactly, one its developer compressed worked out like any other, and a first look never taken for compressed.
    _cache[cacheKey(row)] = format("%lld %lld %lld %lld %lld %d %d %lld %d %d %lld 1 1", (long long)input, (long long)e.outputBytes, (long long)e.marginBytes,
                                   (long long)e.romFsBytes, (long long)e.romFsStoredBytes, e.alreadyCompressed ? 1 : 0, e.measured ? 1 : 0, (long long)e.freedBytes,
                                   e.byMakers ? 1 : 0, e.olderCopy ? 1 : 0, (long long)e.installedBytes);
    _previousAnswers.erase(cacheKey(row));
    // A game read in full took minutes, so what it came to is kept straight away. First looks are kept every few seconds.
    if (e.measured)
        saveCacheLocked();
    else
        saveCacheSoonLocked();
}

/// A failure that would only go the same way again, every time this opened, until it's asked to.
void App::rememberFailureLocked(const Row& row, const std::string& failure) {
    _cache[cacheKey(row)] = RememberedFailure + failure;
    _previousAnswers.erase(cacheKey(row));
    saveCacheSoonLocked();
}

/// Whether what the one before worked out with compression on comes to the same now. A piece of the original game that an update holds more
/// than once is stored once now, which only a game with its update can have. Game data under 1 MB is left as it is now, which DLC often has, and
/// which can make a game with that little, or one that had little or nothing to gain, count as compressed. Nothing that came since makes less of
/// a game count as compressed.
bool App::stillGoodFromBefore(const Row& row, const std::string& answer) const {
    if (row.hasUpdate || !row.updateFile.empty())
        return false;
    long long in = 0, out = 0, margin = 0, romFs = 0, stored = 0;
    int compressed = 0;
    if (sscanf(answer.c_str(), "%lld %lld %lld %lld %lld %d", &in, &out, &margin, &romFs, &stored, &compressed) != 6)
        return false;
    if (compressed)
        return true;
    bool little = romFs > 0 && romFs < LeftAsItIs;
    bool littleToGain = in <= 0 || (double)(in - out) / (double)in < NothingToGain;
    return !row.hasDlc && row.dlcFiles.empty() && !little && !littleToGain;
}

bool App::recallEstimateLocked(Row& row) {
    std::string key = cacheKey(row);
    // One file of a game and its DLC, or of DLC without their game, found damaged as it was saved, isn't written and thrown away again each
    // time this opens, while they go in one file, until its files change or it's analyzed again.
    if (row.tab == TabFiles && _settings.dlcInGameFile && !installsGameFiles()) {
        auto failed = _cache.find(key + OneFileFailure);
        if (failed != _cache.end()) {
            row.state = RowState::Failed;
            row.message = failed->second.substr(strlen(RememberedFailure));
            return false;
        }
    }
    auto remembered = _cache.find(key);
    if (remembered == _cache.end()) {
        // An installed game as it was last time is only known to have DLC or not once it's been looked at again.
        auto before = _previousAnswers.find(key);
        if (before == _previousAnswers.end() || (row.tab == TabInstalled && !row.ready))
            return false;
        std::string answer = std::move(before->second);
        _previousAnswers.erase(before);
        _cacheChanged = true;
        if (!stillGoodFromBefore(row, answer))
            return false;
        remembered = _cache.emplace(key, std::move(answer)).first;
    }
    if (remembered->second.rfind(RememberedFailure, 0) == 0) {
        row.state = RowState::Failed;
        row.message = remembered->second.substr(strlen(RememberedFailure));
        return false;
    }
    long long input, output, margin, romfs, stored, freed = 0, installed = 0;
    int compressed, measured = 0, byMakers = 0, olderCopy = 0, told = 0, asNow = 0;
    int fields = sscanf(remembered->second.c_str(), "%lld %lld %lld %lld %lld %d %d %lld %d %d %lld %d %d", &input, &output, &margin, &romfs, &stored, &compressed,
                        &measured, &freed, &byMakers, &olderCopy, &installed, &told, &asNow);
    if (fields < 6)
        return false;
    // A compressed game remembered from before copies were told apart as they are now is looked at once more: it may be a copy an older
    // version made, which can come out smaller made again, it says what it freed, and copies made before 1.4 were taken for their makers'.
    if (compressed && fields < 12)
        return false;
    // So is an installed one its developer compressed, which is now worked out like any other game, and one only taken for compressed as a
    // first look put a part of it under half a percent, which held game data it hadn't compressed.
    if (compressed && fields < 13 && row.tab == TabInstalled && (byMakers || romfs > 0))
        return false;
    row.hasEstimate = true;
    row.estimate = SizeEstimate{input, output, margin, romfs, stored, compressed != 0};
    row.estimate.measured = measured != 0;
    row.estimate.freedBytes = freed;
    row.estimate.byMakers = byMakers != 0;
    row.estimate.olderCopy = olderCopy != 0;
    if (installed > 0) {
        row.estimate.packedBytes = input;
        row.estimate.installedBytes = installed;
    }
    // Measured before games under 1 GB were measured exactly: its figures stand until it's read once more, in turn.
    row.measureAgain = measured && !compressed && fields < 13 && row.now < MeasureExactlyBelow;
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
        logLine(std::string("无法读取保存的列表： ") + e.what());
        // Turned down by the system, it's read again once the app is open (App::startingUp).
        if (refusedBySystem(e.what()))
            _listRefused = true;
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
                title.madeHere = reader.number() != 0;
                title.originalBytes = reader.big();
                title.solidBytes = reader.big();
                entry.titles.push_back(std::move(title));
            }
            _fileIndex.emplace(std::move(file), std::move(entry));
        }
    } catch (const ToolError& e) {
        logLine(std::string("无法读取游戏文件的已知信息： ") + e.what());
        if (refusedBySystem(e.what()))
            _indexRefused = true;
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
                writer.number(title.madeHere ? 1 : 0);
                writer.big(title.originalBytes);
                writer.big(title.solidBytes);
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
