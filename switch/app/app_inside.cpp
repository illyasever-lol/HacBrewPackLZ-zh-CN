// A game's details, and what it's made of, as + -> This game shows them: the game opened on a thread of its own, its parts as folders, and the
// file under the pointer shown the way its kind is shown: text, a picture, what a folder is made of, or the bytes as they are. Saving any of it
// to the SD card, and finding a file by name.
//
// Each kind of file is shown by its own branch of previewInside, from what its first bytes say it is, so a kind that's only shown as bytes
// today (sounds, textures, archives) gets its own way of being shown by adding one more.
#include "app_internal.h"

#include <cstdio>
#include <map>

#include "../core/nacp.h"
#include "../core/remote.h"

namespace hbp {

using namespace app;

namespace {

/// How much of a file is looked at to tell whether it's text, read to show it as text, and shown as bytes.
constexpr size_t TextLooked = 256 << 10;
constexpr size_t TextRead = 1 << 20;
constexpr size_t BytesShown = 64 << 10;
/// The biggest picture file that's read to be shown.
constexpr i64 PictureMost = 24LL << 20;
constexpr int MostLines = 20000;

std::string lowered(std::string text) {
    for (char& c : text)
        c = (char)tolower((unsigned char)c);
    return text;
}

std::string extensionOf(const std::string& name) {
    size_t dot = name.rfind('.');
    return dot == std::string::npos ? std::string() : lowered(name.substr(dot + 1));
}

bool startsWith(const std::string& text, const char* start) { return text.rfind(start, 0) == 0; }

/// What a file is, in a word, by its name, which is all there is to go by before it's read.
std::string kindByName(const std::string& name) {
    static const std::map<std::string, const char*> Kinds = {
        {"txt", "Text"}, {"json", "Text"}, {"xml", "Text"}, {"ini", "Text"}, {"cfg", "Text"}, {"csv", "Text"}, {"lua", "Text"}, {"js", "Text"},
        {"html", "Text"}, {"htm", "Text"}, {"css", "Text"}, {"yaml", "Text"}, {"yml", "Text"}, {"md", "Text"}, {"log", "Text"}, {"tsv", "Text"},
        {"glsl", "Text"}, {"hlsl", "Text"}, {"shader", "Text"}, {"py", "Text"}, {"cs", "Text"}, {"toml", "Text"}, {"properties", "Text"},
        {"png", "Picture"}, {"jpg", "Picture"}, {"jpeg", "Picture"}, {"bmp", "Picture"}, {"tga", "Picture"}, {"gif", "Picture"}, {"webp", "Picture"},
        {"dds", "Texture"}, {"bntx", "Texture"}, {"bflim", "Texture"}, {"ktx", "Texture"}, {"astc", "Texture"}, {"xtx", "Texture"},
        {"bfstm", "Sound"}, {"bfwav", "Sound"}, {"bwav", "Sound"}, {"bcstm", "Sound"}, {"bars", "Sound"}, {"ogg", "Sound"}, {"wav", "Sound"},
        {"mp3", "Sound"}, {"opus", "Sound"}, {"adx", "Sound"}, {"hca", "Sound"}, {"wem", "Sound"}, {"bnk", "Sound"}, {"fsb", "Sound"},
        {"bank", "Sound"}, {"acb", "Sound"}, {"awb", "Sound"}, {"flac", "Sound"},
        {"mp4", "Video"}, {"webm", "Video"}, {"usm", "Video"}, {"bk2", "Video"}, {"mov", "Video"}, {"avi", "Video"}, {"mkv", "Video"},
        {"sarc", "Archive"}, {"szs", "Archive"}, {"zs", "Archive"}, {"zip", "Archive"}, {"arc", "Archive"}, {"narc", "Archive"},
        {"bfarc", "Archive"}, {"rarc", "Archive"}, {"pack", "Archive"}, {"cpk", "Archive"}, {"bundle", "Archive"}, {"assets", "Archive"},
        {"resource", "Archive"}, {"pak", "Archive"}, {"7z", "Archive"}, {"gz", "Archive"}, {"lz4", "Archive"}, {"bea", "Archive"},
        {"ttf", "Font"}, {"otf", "Font"}, {"bfttf", "Font"}, {"bfotf", "Font"}, {"bffnt", "Font"}, {"fnt", "Font"},
        {"bfres", "Model"}, {"fbx", "Model"}, {"obj", "Model"}, {"gltf", "Model"}, {"glb", "Model"},
        {"nso", "Program"}, {"nro", "Program"}, {"nrr", "Program"}, {"npdm", "Program"}, {"so", "Program"}, {"dll", "Program"},
    };
    auto found = Kinds.find(extensionOf(name));
    if (found != Kinds.end())
        return found->second;
    // The program's own files have no extension: main, rtld, sdk and subsdk0 to 9.
    if (name == "main" || name == "rtld" || name == "sdk" || startsWith(name, "subsdk"))
        return "Program";
    // A game's icons are JPEGs, named for the language they're for.
    if (startsWith(name, "icon_") && extensionOf(name) == "dat")
        return "Picture";
    return "Data";
}

/// What its first bytes say it is, which goes before its name: a file named as a picture that isn't one is data, whatever its name says.
std::string kindByBytes(const u8* data, size_t size, const std::string& byName, const std::string& name) {
    if (size >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
        return "Picture";
    if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
        return "Picture";
    if (size >= 2 && data[0] == 'B' && data[1] == 'M' && byName != "Text")
        return "Picture";
    if ((size >= 4 && memcmp(data, "GIF8", 4) == 0) || (size >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0))
        return "Picture";
    // TGA pictures don't start with anything that says so.
    if (byName == "Picture" && extensionOf(name) != "tga")
        return "Data";
    return byName;
}

/// Whether bytes read as text: UTF-8, with no NULs and few other control characters.
bool readsAsText(const u8* data, size_t size) {
    if (size == 0)
        return false;
    size_t odd = 0;
    for (size_t i = 0; i < size;) {
        u8 c = data[i];
        if (c == 0)
            return false;
        if (c < 0x80) {
            if (c < 0x20 && c != '\n' && c != '\r' && c != '\t' && c != '\f')
                odd++;
            i++;
            continue;
        }
        int more = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (more < 0)
            return false;
        // A character cut off where the read stopped is still text.
        for (int k = 1; k <= more; k++) {
            if (i + (size_t)k >= size)
                return true;
            if ((data[i + (size_t)k] & 0xC0) != 0x80)
                return false;
        }
        i += (size_t)more + 1;
    }
    return odd * 50 < size;
}

bool isUtf16(const Bytes& data) { return data.size() >= 2 && ((data[0] == 0xFF && data[1] == 0xFE) || (data[0] == 0xFE && data[1] == 0xFF)); }

/// Text kept as UTF-16, which some games' text is, as UTF-8, which is what's drawn.
Bytes fromUtf16(const Bytes& data) {
    bool big = data[0] == 0xFE;
    Bytes out;
    out.reserve(data.size());
    auto put = [&](u32 c) {
        if (c < 0x80) {
            out.push_back((u8)c);
        } else if (c < 0x800) {
            out.push_back((u8)(0xC0 | c >> 6));
            out.push_back((u8)(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back((u8)(0xE0 | c >> 12));
            out.push_back((u8)(0x80 | (c >> 6 & 0x3F)));
            out.push_back((u8)(0x80 | (c & 0x3F)));
        } else {
            out.push_back((u8)(0xF0 | c >> 18));
            out.push_back((u8)(0x80 | (c >> 12 & 0x3F)));
            out.push_back((u8)(0x80 | (c >> 6 & 0x3F)));
            out.push_back((u8)(0x80 | (c & 0x3F)));
        }
    };
    for (size_t i = 2; i + 1 < data.size(); i += 2) {
        u32 c = big ? (u32)data[i] << 8 | data[i + 1] : (u32)data[i + 1] << 8 | data[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < data.size()) {
            u32 low = big ? (u32)data[i + 2] << 8 | data[i + 3] : (u32)data[i + 3] << 8 | data[i + 2];
            if (low >= 0xDC00 && low < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }
        if (c != 0)
            put(c);
    }
    return out;
}

/// Its lines, as many as there's any point in, with tabs as spaces.
std::vector<std::string> linesOf(const u8* data, size_t size, int& total) {
    std::vector<std::string> lines;
    std::string line;
    total = 0;
    // A byte-order mark isn't anything to show.
    size_t i = size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF ? 3 : 0;
    for (; i < size; i++) {
        char c = (char)data[i];
        if (c == '\r')
            continue;
        if (c == '\n') {
            total++;
            if ((int)lines.size() < MostLines)
                lines.push_back(std::move(line));
            line.clear();
        } else if (c == '\t') {
            line.append(4 - line.size() % 4, ' ');
        } else if ((unsigned char)c >= 0x20) {
            if (line.size() < 400)
                line += c;
        }
    }
    if (!line.empty()) {
        total++;
        if ((int)lines.size() < MostLines)
            lines.push_back(std::move(line));
    }
    return lines;
}

/// Why, in a few plain words where there are some, or as it was said: the details are where the rest is read.
std::string whyItFailed(const std::string& failure) {
    std::string plain = plainReason(failure);
    return plain.rfind("出错了", 0) == 0 ? failure : plain;
}

/// The game's parts, named for what they're for.
std::string partName(const ContentPair& content, int section, const NcaFsSection& fs) {
    std::string name;
    switch (content.type) {
        case Cnmt::ContentTypeProgram:
            name = section == 1 ? "游戏数据" : section == 2 ? "启动图标" : fs.fsType() == NcaFsSection::FsPartition ? "程序文件" : format("第 %d 部分", section);
            break;
        case Cnmt::ContentTypeControl:
            name = "图标与名称";
            break;
        case Cnmt::ContentTypeHtmlDocument:
            name = "说明书";
            break;
        case Cnmt::ContentTypeLegalInformation:
            name = "法律声明";
            break;
        case Cnmt::ContentTypeData:
            name = "数据";
            break;
        default:
            name = format("第 %d 部分", content.type);
            break;
    }
    if (content.idOffset != 0)
        name += format(" %d", content.idOffset + 1);
    return name;
}

/// The languages a game has its name in, in the NACP's own order.
const char* const LanguageNames[] = {"English", "English (UK)", "Japanese", "French", "German", "Spanish (Latin America)", "Spanish", "Italian",
                                     "Dutch", "French (Canada)", "Portuguese", "Russian", "Korean", "Chinese (Traditional)", "Chinese (Simplified)",
                                     "Portuguese (Brazil)"};

/// The first system version each key generation came with, which the keys it takes are dumped from.
std::string firstSystemWith(int generation) {
    static const char* const First[] = {"1.0.0", "3.0.0", "3.0.1", "4.0.0", "5.0.0", "6.0.0", "6.2.0", "7.0.0", "8.1.0", "9.0.0", "9.1.0",
                                        "12.1.0", "13.0.0", "14.0.0", "15.0.0", "16.0.0", "17.0.0", "18.0.0", "19.0.0", "20.0.0", "21.0.0"};
    return generation >= 0 && generation < (int)(sizeof(First) / sizeof(First[0])) ? First[generation] : std::string("较新的版本");
}

/// How big a picture is, from its header, before it's decoded: false when there's no telling.
bool pictureSize(const Bytes& data, int& width, int& height) {
    if (data.size() >= 24 && data[0] == 0x89 && data[1] == 'P') {
        width = (int)(data[16] << 24 | data[17] << 16 | data[18] << 8 | data[19]);
        height = (int)(data[20] << 24 | data[21] << 16 | data[22] << 8 | data[23]);
        return width > 0 && height > 0;
    }
    if (data.size() >= 26 && data[0] == 'B' && data[1] == 'M') {
        width = (int)rd32(data.data() + 18);
        height = std::abs((int)rd32(data.data() + 22));
        return width > 0 && height > 0;
    }
    return jpegSize(data, width, height);
}

/// A picture made to fit somewhere: smaller, keeping its shape, or a small one bigger by whole steps, up to four times, so it stays sharp.
std::shared_ptr<Image> fitted(const Image& image, int width, int height) {
    auto out = std::make_shared<Image>();
    if (image.empty())
        return out;
    if (image.w > width || image.h > height) {
        double scale = std::min((double)width / image.w, (double)height / image.h);
        *out = image.scaledTo(std::max(1, (int)(image.w * scale)), std::max(1, (int)(image.h * scale)));
        return out;
    }
    int times = std::max(1, std::min({width / image.w, height / image.h, 4}));
    if (times == 1) {
        *out = image;
        return out;
    }
    out->w = image.w * times;
    out->h = image.h * times;
    out->pixels.resize((size_t)out->w * out->h);
    for (int y = 0; y < out->h; y++) {
        for (int x = 0; x < out->w; x++)
            out->pixels[(size_t)y * out->w + x] = image.pixels[(size_t)(y / times) * image.w + x / times];
    }
    return out;
}

}  // namespace

bool App::insideLoadingLocked() const {
    if (!_inside || (_screen != Screen::Details && _screen != Screen::Inside))
        return false;
    const Inside& inside = *_inside;
    return !inside.ready || inside.saving ||
           (_screen == Screen::Inside && inside.problem.empty() && _insideWant >= 0 && (inside.previewOf != _insideWant || !inside.previewReady));
}

bool App::canLookInside(const Row& row) const {
    // An extracted game is a folder already, and an update file for an installed game is that game, which its own row opens.
    if (!row.extracted.empty() || row.overInstalled)
        return false;
    return row.tab == TabInstalled ? _console != nullptr : !row.mainFile.empty();
}

const std::vector<int>& App::insideShownLocked() const {
    const Inside& inside = *_inside;
    return inside.foundFrom >= 0 ? inside.found : inside.nodes[(size_t)inside.folder].children;
}

void App::openInside(bool details) {
    Row row;
    {
        sys::Lock lock(_mutex);
        auto shown = visibleRowsLocked(_tab);
        int index = _focus[_tab];
        if (index < 0 || index >= (int)shown.size())
            return;
        row = *shown[(size_t)index];
    }
    if (!canLookInside(row))
        return;
    closeInside();
    auto inside = std::make_unique<Inside>();
    inside->key = row.key;
    inside->tab = row.tab;
    inside->name = row.name;
    inside->version = row.version;
    inside->icon = row.icon;
    inside->tile = row.tile;
    inside->fromDetails = details;
    // The list's icon until the game's own is read.
    if (row.icon && !row.icon->empty()) {
        inside->iconBig = std::make_shared<Image>(row.icon->scaledTo(176, 176));
        inside->iconSmall = std::make_shared<Image>(row.icon->scaledTo(36, 36));
    }
    // What About this game said: what stops it, or what went wrong with it, first.
    inside->notes = row.notes;
    if (!row.blocked.empty())
        inside->notes.insert(inside->notes.begin(), row.blocked);
    else if (row.state == RowState::Failed && !row.message.empty())
        inside->notes.insert(inside->notes.begin(), row.message);
    else if (row.updateOverCopy && row.state != RowState::Done)
        inside->notes.insert(inside->notes.begin(), UpdateToMakePartAbout);
    {
        sys::Lock lock(_mutex);
        _inside = std::move(inside);
        _insideStop = false;
        _insideWant = -1;
        _insidePreviewing = -1;
        _insideSave = -1;
        _insideSaveStop = false;
    }
    _menu.reset();
    _glideList = -1;
    _screen = details ? Screen::Details : Screen::Inside;
    _insideThread = std::make_unique<sys::Thread>();
    _insideThread->start([this, row] {
        // A PC that's slow to answer gives way as soon as Look inside is closed, or what's being read is no longer what's under the pointer.
        remote::setGiveWay([this] {
            int previewing = _insidePreviewing;
            return _insideStop.load() || (previewing >= 0 && previewing != _insideWant);
        });
        insideReaderFor(row);
    }, -1, 1 << 20);
    _dirty = true;
}

void App::closeInside() {
    if (_insideThread) {
        {
            sys::Lock lock(_mutex);
            _insideStop = true;
            _insideSaveStop = true;
            _insideWake.wakeAll();
        }
        _insideThread->join();
        _insideThread.reset();
    }
    sys::Lock lock(_mutex);
    _inside.reset();
    if (_screen == Screen::Details || _screen == Screen::Inside)
        _screen = Screen::Main;
    _dirty = true;
}

/// The game read, then what's asked for, the newest first, until it's closed.
void App::insideReaderFor(const Row& row) {
    Inside read;
    try {
        readInside(read, row);
    } catch (const Cancelled&) {
        if (_insideStop)
            return;
        read.problem = "已停止打开";
    } catch (const ToolError& e) {
        read.problem = e.what();
    } catch (const std::bad_alloc&) {
        read.problem = "内存不足，无法打开";
    }
    // The details say what they can even when it couldn't be opened, from what the list knows.
    try {
        detailsOf(read, row);
    } catch (const Cancelled&) {
        if (_insideStop)
            return;
    } catch (const ToolError&) {
    } catch (const std::bad_alloc&) {
    }
    bool opened = read.problem.empty();
    {
        sys::Lock lock(_mutex);
        if (_insideStop || !_inside)
            return;
        Inside& inside = *_inside;
        inside.problem = read.problem;
        if (!read.publisher.empty())
            inside.publisher = read.publisher;
        inside.stem = read.stem;
        inside.set = std::move(read.set);
        inside.parts = std::move(read.parts);
        inside.nodes = std::move(read.nodes);
        inside.files = read.files;
        inside.bytes = read.bytes;
        inside.stateLabel = read.stateLabel;
        inside.stateBig = read.stateBig;
        inside.stateSub = read.stateSub;
        inside.shareWord = read.shareWord;
        inside.share = read.share;
        inside.sections = std::move(read.sections);
        if (read.iconBig)
            inside.iconBig = read.iconBig;
        if (read.iconSmall)
            inside.iconSmall = read.iconSmall;
        if (inside.nodes.empty())
            inside.nodes.push_back(InsideNode{inside.name, true});
        inside.ready = true;
        // What the pointer starts on is shown straight away.
        auto& shown = insideShownLocked();
        _insideWant = shown.empty() || !opened ? -1 : shown[0];
        _dirty = true;
    }
    if (!opened)
        return;
    for (;;) {
        int want, save;
        {
            sys::Lock lock(_mutex);
            while (!_insideStop && _insideSave < 0 && (!_inside || _insideWant < 0 || _insideWant == _inside->previewOf))
                _insideWake.wait(_mutex);
            if (_insideStop)
                return;
            want = _insideWant;
            save = _insideSave;
            _insideSave = -1;
        }
        // What's shown when what's under the pointer can't be: why, in a few words.
        auto unshown = [&](const std::string& kind, const std::string& why) {
            sys::Lock lock(_mutex);
            if (!_inside || _insideWant != want)
                return;
            Inside& inside = *_inside;
            inside.previewOf = want;
            inside.previewReady = true;
            inside.previewKind = kind;
            inside.chips = {why};
            inside.lines.clear();
            inside.raw.clear();
            inside.rawShown = false;
            inside.picture.reset();
            inside.pictureFull.reset();
            inside.breakdown.clear();
            _dirty = true;
        };
        auto saveEnded = [&](const std::string& toast) {
            sys::Lock lock(_mutex);
            if (_inside)
                _inside->saving = false;
            _toast = toast;
            _toastUntil = _time + 4;
            _dirty = true;
        };
        try {
            if (save >= 0) {
                saveInside(save);
            } else {
                _insidePreviewing = want;
                previewInside(*_inside, want);
            }
        } catch (const Cancelled&) {
            if (_insideStop)
                return;
            if (save >= 0)
                saveEnded("已停止保存");
            // Given up on as the pointer moved on, the next one is read next. Stopped any other way, it says so.
            else if (_insideWant == want)
                unshown("Unreadable", "已停止读取");
        } catch (const ToolError& e) {
            if (save >= 0)
                saveEnded("保存失败：" + whyItFailed(e.what()));
            else
                unshown("Unreadable", whyItFailed(e.what()));
        } catch (const std::bad_alloc&) {
            if (save >= 0)
                saveEnded("保存失败：内存不足");
            else
                unshown("内容过大，无法显示", "内存不足，无法显示");
        }
        _insidePreviewing = -1;
    }
}

void App::readInside(Inside& in, const Row& row) {
    auto stopIfClosed = [this] {
        if (_insideStop)
            throw Cancelled();
    };
    auto set = std::make_shared<TitleSet>(_keyset);
    if (row.tab == TabInstalled) {
        InstalledGame game = _console->describe(row.application);
        // What stops it being compressed doesn't stop it being looked at, unless there's nothing of it there.
        if (game.metas.empty())
            throw ToolError(game.blocked.empty() ? std::string("其中没有已安装的内容") : game.blocked);
        _console->addTitleKeys(set->tickets(), _keyset);
        set->add(std::make_shared<InstalledArchive>(*_console, game));
        in.publisher = game.publisher;
    } else {
        std::set<std::string> added;
        for (const std::string* path : {&row.mainFile, &row.updateFile}) {
            if (!path->empty() && added.insert(*path).second)
                set->add(archiveFor(*path));
        }
        for (auto& dlc : row.dlcFiles) {
            if (added.insert(dlc).second)
                set->add(archiveFor(dlc));
        }
    }
    stopIfClosed();
    set->group(true);
    const TitleJob* main = nullptr;
    std::vector<const TitleJob*> dlcs;
    for (auto& job : set->jobs) {
        if (job.main->meta.type == Cnmt::MetaAddOnContent)
            dlcs.push_back(&job);
        else if (!main)
            main = &job;
    }
    if (!main && !dlcs.empty()) {
        main = dlcs[0];
        dlcs.erase(dlcs.begin());
    }
    if (!main)
        throw ToolError(set->problems.empty() ? std::string("其中没有可读取的内容") : set->problems[0].message);
    u32 version = main->update ? main->update->meta.version : main->main->meta.version;
    in.stem = main->stem(version, false);

    // The top, then each part as a folder of its own.
    in.nodes.clear();
    in.nodes.push_back(InsideNode{row.name, true});
    std::map<std::pair<int, std::string>, int> folders;
    auto folderIn = [&](int parent, const std::string& name) {
        auto key = std::make_pair(parent, name);
        auto found = folders.find(key);
        if (found != folders.end())
            return found->second;
        InsideNode node;
        node.name = name;
        node.folder = true;
        node.parent = parent;
        node.kind = "Folder";
        int index = (int)in.nodes.size();
        in.nodes.push_back(std::move(node));
        in.nodes[(size_t)parent].children.push_back(index);
        folders[key] = index;
        return index;
    };
    auto fileIn = [&](int parent, const std::string& name, int part, i64 offset, i64 size) {
        InsideNode node;
        node.name = name;
        node.parent = parent;
        node.part = part;
        node.offset = offset;
        node.size = size;
        node.kind = kindByName(name);
        int index = (int)in.nodes.size();
        in.nodes.push_back(std::move(node));
        in.nodes[(size_t)parent].children.push_back(index);
    };
    auto addSection = [&](const ContentPair& content, int section, const std::string& prefix) {
        stopIfClosed();
        SectionSource source = {};
        if (!content.sectionAt(section, source))
            return;
        // A DLC's data is all a DLC is, so it's called that alone.
        std::string own = partName(content, section, *source.section);
        std::string name = prefix.empty() ? own : own == "Data" ? prefix : prefix + " " + own;
        int part = (int)in.parts.size();
        int top = folderIn(0, name);
        if (source.section->fsType() == NcaFsSection::FsPartition) {
            auto archive = source.owner->openPfs0(*source.section, source.original);
            in.parts.emplace_back(name, archive->data());
            for (auto& file : archive->files())
                fileIn(top, file.name, part, file.offset, file.size);
            return;
        }
        SourcePtr image = source.owner->openRomFsImage(*source.section, source.original);
        RomFsArchive archive = RomFsArchive::open(image, source.owner->name());
        in.parts.emplace_back(name, image);
        auto placeOf = [&](const std::string& path, bool isFolder, std::string& last) {
            int at = top;
            size_t start = path.size() > 0 && path[0] == '/' ? 1 : 0;
            while (start < path.size()) {
                size_t slash = path.find('/', start);
                std::string piece = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
                if (slash == std::string::npos && !isFolder) {
                    last = piece;
                    return at;
                }
                if (!piece.empty())
                    at = folderIn(at, piece);
                if (slash == std::string::npos)
                    break;
                start = slash + 1;
            }
            return at;
        };
        std::string last;
        for (auto& folder : archive.directories)
            placeOf(folder, true, last);
        for (auto& file : archive.files) {
            int parent = placeOf(file.path, false, last);
            fileIn(parent, last, part, file.offset, file.size);
        }
    };
    for (auto& content : main->contents) {
        for (int section : content.sectionsInFileOrder())
            addSection(content, section, "");
    }
    for (size_t i = 0; i < dlcs.size(); i++) {
        for (auto& content : dlcs[i]->contents) {
            for (int section : content.sectionsInFileOrder())
                addSection(content, section, dlcs.size() == 1 ? std::string("DLC") : format("DLC %d", (int)i + 1));
        }
    }

    // The parts in the same order whatever order a game keeps them in: what most of it is first, its DLC last.
    auto rank = [&](int index) {
        static const char* const Order[] = {"游戏数据", "程序文件", "图标与名称", "说明书", "法律声明", "启动图标", "数据", "第"};
        const std::string& name = in.nodes[(size_t)index].name;
        for (int i = 0; i < (int)(sizeof(Order) / sizeof(Order[0])); i++) {
            if (startsWith(name, Order[i]))
                return i;
        }
        return 100;
    };
    std::stable_sort(in.nodes[0].children.begin(), in.nodes[0].children.end(), [&](int a, int b) { return rank(a) < rank(b); });

    // What each folder holds, and its own in order: folders first, then files, each by name.
    for (size_t i = in.nodes.size(); i-- > 0;) {
        InsideNode& node = in.nodes[i];
        if (!node.folder) {
            in.files++;
            in.bytes += node.size;
            continue;
        }
        node.size = 0;
        node.items = 0;
        for (int child : node.children) {
            node.size += in.nodes[(size_t)child].size;
            node.items++;
        }
    }
    for (size_t i = 1; i < in.nodes.size(); i++) {
        InsideNode& node = in.nodes[i];
        if (!node.folder)
            continue;
        std::sort(node.children.begin(), node.children.end(), [&](int a, int b) {
            const InsideNode& x = in.nodes[(size_t)a];
            const InsideNode& y = in.nodes[(size_t)b];
            if (x.folder != y.folder)
                return x.folder;
            return lessNoCase(x.name, y.name);
        });
    }
    in.set = set;
}

/// What the details say: the one figure that decides, and the facts in four places that are always the same.
void App::detailsOf(Inside& in, const Row& row) {
    const TitleJob* main = nullptr;
    int dlcs = 0;
    if (in.set) {
        for (auto& job : in.set->jobs) {
            if (job.main->meta.type == Cnmt::MetaAddOnContent)
                dlcs++;
            else if (!main)
                main = &job;
        }
    }
    // The names and languages, from its control data, which isn't worth giving up the rest for.
    std::vector<NacpTitle> names;
    std::string displayVersion;
    try {
        for (size_t p = 0; p < in.parts.size() && names.empty(); p++) {
            if (in.parts[p].first != "图标与名称")
                continue;
            for (auto& node : in.nodes) {
                if (node.part == (int)p && !node.folder && node.name == "control.nacp" && node.size >= (i64)Nacp::Size) {
                    Bytes nacp((size_t)Nacp::Size);
                    in.parts[p].second->read(node.offset, nacp.data(), nacp.size());
                    names = Nacp::titles(nacp.data(), nacp.size());
                    displayVersion = Nacp::displayVersion(nacp.data(), nacp.size());
                }
            }
        }
    } catch (const ToolError&) {
    }
    if (!names.empty() && in.publisher.empty())
        in.publisher = Nacp::choose(names).publisher;
    // Its icon as the game has it, which the list only keeps small: the American English one, as the list's is, or the first there is.
    try {
        const InsideNode* best = nullptr;
        for (auto& node : in.nodes) {
            if (node.folder || node.part < 0 || in.parts[(size_t)node.part].first != "图标与名称" || !startsWith(node.name, "icon_") || node.size > (1 << 20))
                continue;
            if (!best || node.name == "icon_AmericanEnglish.dat")
                best = &node;
        }
        Image icon;
        if (best) {
            Bytes jpeg((size_t)best->size);
            in.parts[(size_t)best->part].second->read(best->offset, jpeg.data(), jpeg.size());
            if (_platform.decodeJpeg(jpeg, icon) && !icon.empty()) {
                in.iconBig = std::make_shared<Image>(icon.scaledTo(176, 176));
                in.iconSmall = std::make_shared<Image>(icon.scaledTo(36, 36));
            }
        }
    } catch (const ToolError&) {
    }
    std::vector<std::string> languages;
    for (size_t i = 0; i < names.size() && i < sizeof(LanguageNames) / sizeof(LanguageNames[0]); i++) {
        // British English is English too, as far as anyone choosing a game goes.
        if (!names[i].name.empty() && !(i == 1 && !names[0].name.empty()))
            languages.push_back(LanguageNames[i]);
    }
    bool compressed = isCompressed(row);

    // What's wrong comes first, in a few words with what to do.
    DetailsSection wrong{"问题所在"};
    if (!in.problem.empty()) {
        wrong.lines.push_back({"Opening it", "无法完成", whyItFailed(in.problem)});
        wrong.lines.back().bad = true;
    }
    DetailsSection inIt{"内容"};
    std::string version = !displayVersion.empty() ? displayVersion : row.version;
    if (main) {
        bool updated = main->update != nullptr;
        bool merged = !updated && main->main->meta.version > 0 && row.tab == TabInstalled;
        inIt.lines.push_back({"游戏本体", version.empty() ? format("v%u", main->main->meta.version) : version,
                              updated ? "含更新补丁" : merged ? "已整合更新补丁" : ""});
        inIt.lines.push_back({"DLC", dlcs == 0 ? std::string("无") : format("%d", dlcs)});
    } else if (!version.empty()) {
        inIt.lines.push_back({"游戏本体", version});
    }
    if (!languages.empty()) {
        std::string listed;
        size_t shown = std::min<size_t>(languages.size(), 4);
        for (size_t i = 0; i < shown; i++)
            listed += (i == 0 ? "" : i + 1 == shown && shown == languages.size() ? " 和 " : "、") + languages[i];
        if (languages.size() > shown)
            listed += format(" 等 %d 种", (int)(languages.size() - shown));
        inIt.lines.push_back({"语言", listed});
    }
    inIt.lines.push_back({"游戏 ID", format("%016llX", (unsigned long long)row.application)});

    // Where it is and what it takes.
    DetailsSection place;
    if (row.tab == TabInstalled) {
        place.title = "在主机上";
        i64 asSold = compressed && row.hasEstimate && row.estimate.freedBytes > 0 ? row.now + row.estimate.freedBytes : 0;
        place.lines.push_back({"位置", row.storage == Storage::Nand ? "主机自带存储" : row.storage == Storage::Sd ? "SD 卡" : "游戏卡带",
                               asSold > 0 ? formatSize(asSold) + " 原版容量" : ""});
        if (compressed && row.hasEstimate && row.estimate.byMakers)
            place.lines.push_back({"已压缩", "由开发者压缩"});
    } else {
        place.title = "文件";
        std::string ext = extensionOf(row.mainFile);
        bool card = isCardPath(row.mainFile);
        std::string kind = card ? "游戏卡带上" : ext == "nsz" ? "NSZ" : ext == "xci" ? "XCI" : ext == "xcz" ? "XCZ" : ext == "nsp" ? "NSP" : "游戏文件";
        std::string what = card ? "经主机读取，保持原样"
                         : ext == "nsz" || ext == "xcz" ? "打包存放，安装时解包"
                         : ext == "xci" ? "游戏卡带副本" : "与售卖版本相同";
        place.lines.push_back({"类型", kind, what});
        i64 packed = row.hasEstimate && row.estimate.packedBytes > 0 ? row.estimate.packedBytes : 0;
        place.lines.push_back({"大小", formatSize(packed > 0 ? packed : row.now), packed > 0 ? formatSize(row.estimate.inputBytes) + " 按原样安装" : ""});
        std::string where = whereIs(row.mainFile);
        // "在 " 是 4 个 UTF-8 字节（汉字 3 + 空格 1），沿用英文 "On " 的 substr(3) 会切出半个汉字。
        if (startsWith(where, "在 "))
            where = where.substr(sizeof("在 ") - 1);
        place.lines.push_back({"位置", where.empty() ? std::string("此处") : where, card ? "" : row.mainFile});
    }

    // What it needs: the system, keys, and a ticket.
    DetailsSection needs{"运行要求"};
    if (main) {
        u32 required = main->main->meta.requiredSystemVersion();
        if (main->update)
            required = std::max(required, main->update->meta.requiredSystemVersion());
        u32 console = _platform.systemVersion();
        if (console >= (12u << 26) && required > console) {
            wrong.lines.push_back({"系统版本", "需要 " + versionName(required) + "，当前为 " + versionName(console), "请升级主机系统后再运行"});
            wrong.lines.back().bad = true;
        } else {
            needs.lines.push_back({"系统版本", required == 0 ? std::string("任意版本") : versionName(required) + " 或更高",
                                   console >= (12u << 26) ? "当前为 " + versionName(console) : ""});
        }
        int generation = 0;
        bool rights = false;
        for (auto& content : main->contents) {
            generation = std::max(generation, content.primary().generationIndex());
            rights = rights || content.primary().hasRightsId();
        }
        bool ours = rights ? _keyset.titleKek(generation) != nullptr : _keyset.keyAreaKey(0, generation) != nullptr;
        if (!ours) {
            wrong.lines.push_back({"密钥", "密钥过旧：需要 " + firstSystemWith(generation) + " 及更高系统版本的密钥",
                                   "升级系统后重新运行 Lockpick_RCM"});
            wrong.lines.back().bad = true;
        }
    } else {
        needs.lines.push_back({"未知", "无法打开以确认"});
    }
    if (compressed && !(row.hasEstimate && row.estimate.byMakers))
        needs.lines.push_back({"启动所需", "签名补丁", "与所有压缩游戏相同"});

    // What compressing it does, or did.
    DetailsSection compressing;
    if (compressed) {
        compressing.title = row.tab == TabInstalled ? "更新" : "压缩它";
        if (row.tab == TabInstalled && row.hasEstimate && row.estimate.byMakers)
            compressing.lines.push_back({"下次更新", "按常规安装", "由开发者压缩，不做改动"});
        else if (row.tab == TabInstalled)
            compressing.lines.push_back({"下次更新", "在此整合进本体", "直接覆盖安装会导致无法启动"});
        else
            compressing.lines.push_back({"已完成", "已压缩", "安装时直接复制"});
    } else {
        compressing.title = "压缩它";
        if (_settings.compress)
            compressing.lines.push_back({"处理后", "需要签名补丁才能启动", "与所有压缩游戏相同"});
    }
    // What's wrong goes at the top of what it needs, which then comes first, so the four boxes there's room for are enough.
    if (!wrong.lines.empty()) {
        wrong.lines.insert(wrong.lines.end(), needs.lines.begin(), needs.lines.end());
        in.sections = {wrong, inIt, place, compressing};
    } else {
        in.sections = {inIt, place, needs, compressing};
    }

    // The one figure that decides, on the left: what it's said to be, the figure, and a line saying what the figure is beside.
    in.share = -1;
    in.shareWord.clear();
    in.stateSub.clear();
    bool installed = row.tab == TabInstalled;
    i64 asIs = row.hasEstimate && row.estimate.packedBytes > 0 ? row.estimate.inputBytes : row.now;
    if (compressed && row.hasEstimate && row.estimate.freedBytes > 0) {
        in.stateLabel = installed ? "压缩后释放" : "压缩后缩小";
        in.stateBig = formatSize(row.estimate.freedBytes);
        in.stateSub = "当前占用 " + formatSize(row.now);
        in.share = (double)row.now / (double)(row.now + row.estimate.freedBytes);
        in.shareWord = "已节省";
    } else if (compressed) {
        in.stateLabel = row.hasEstimate && row.estimate.byMakers ? "由开发者压缩，占用" : "压缩后占用";
        in.stateBig = formatSize(row.now);
    } else if (row.hasEstimate && !row.notSure && row.estimate.sureSavedBytes() > 0 && row.belowMinimum.empty()) {
        in.stateLabel = installed ? "压缩后将占用" : "压缩安装后将占用";
        in.stateBig = formatSize(row.estimate.mostBytes());
        in.stateSub = "原本为 " + formatSize(asIs);
        in.share = 1 - row.estimate.sureSavedFraction();
        in.shareWord = "节省";
    } else if (row.hasEstimate && row.notSure) {
        in.stateLabel = "压缩可节省";
        in.stateBig = "尚未确定";
        in.stateSub = "完整测量结果为";
    } else if (row.hasEstimate) {
        in.stateLabel = installed ? "保持原样占用" : "按原样安装占用";
        in.stateBig = formatSize(asIs);
        in.stateSub = "压缩收益太小";
    } else {
        in.stateLabel = "压缩可节省";
        in.stateBig = "尚未计算";
    }
}

/// What's under the pointer, read and made ready to show the way its kind is shown.
void App::previewInside(Inside& inside, int index) {
    const InsideNode& node = inside.nodes[(size_t)index];
    std::string kind = node.kind;
    std::vector<std::string> chips, lines;
    std::vector<Inside::KindCount> breakdown;
    int total = 0;
    Bytes raw;
    bool rawShown = false;
    std::shared_ptr<Image> picture, pictureFull;
    bool asBytes;
    {
        sys::Lock lock(_mutex);
        asBytes = inside.asBytes;
    }
    auto readAll = [&](const InsideNode& file, size_t size) {
        Bytes data(size);
        if (size > 0)
            inside.parts[(size_t)file.part].second->read(file.offset, data.data(), size);
        return data;
    };
    // With little memory, as when the app runs in place of the album, a picture bigger than a screen and a bit is left as bytes, rather than
    // taking more memory than there is to decode it.
    bool limited = _platform.memoryIsLimited();
    auto decode = [&](const Bytes& data, int& width, int& height, int fitW, int fitH) {
        int w = 0, h = 0;
        if (limited && pictureSize(data, w, h) && (i64)w * h > 2048LL * 2048)
            return false;
        Image image;
        if (!(decodePng(data, image) || decodeBmp(data, image) || _platform.decodeJpeg(data, image)))
            return false;
        width = image.w;
        height = image.h;
        picture = fitted(image, fitW, fitH);
        pictureFull = fitted(image, InsideFullPictureW, InsideFullPictureH);
        return true;
    };
    if (node.folder) {
        kind = "Folder";
        chips = {"文件夹", itemsWord(node.items), formatSize(node.size)};
        // What it's made of, all the way down, by kind, the most first.
        std::map<std::string, Inside::KindCount> kinds;
        std::vector<int> stack(node.children.begin(), node.children.end());
        while (!stack.empty()) {
            const InsideNode& c = inside.nodes[(size_t)stack.back()];
            stack.pop_back();
            if (c.folder) {
                stack.insert(stack.end(), c.children.begin(), c.children.end());
                continue;
            }
            Inside::KindCount& count = kinds[c.kind];
            count.kind = c.kind;
            count.files++;
            count.bytes += c.size;
        }
        for (auto& count : kinds)
            breakdown.push_back(count.second);
        std::sort(breakdown.begin(), breakdown.end(), [](const Inside::KindCount& a, const Inside::KindCount& b) { return a.bytes > b.bytes; });
        // And the first picture straight inside it, such as a game's icon, which says more than any count.
        for (int child : node.children) {
            const InsideNode& c = inside.nodes[(size_t)child];
            if (c.folder || c.kind != "Picture" || c.size > PictureMost)
                continue;
            int width = 0, height = 0;
            if (decode(readAll(c, (size_t)c.size), width, height, InsidePictureW, InsideFolderPictureH)) {
                chips.push_back("显示 " + c.name);
                break;
            }
        }
    } else if (node.size == 0) {
        chips = {kind, "空"};
    } else {
        size_t want = (size_t)std::min<i64>(node.size, (i64)TextRead);
        if (node.kind == "Picture" && node.size <= PictureMost)
            want = (size_t)node.size;
        Bytes data = readAll(node, want);
        kind = kindByBytes(data.data(), data.size(), node.kind, node.name);
        chips = {kind, formatSize(node.size)};
        int width = 0, height = 0;
        if (!asBytes && kind == "Picture" && (i64)want == node.size && decode(data, width, height, InsidePictureW, InsidePictureH)) {
            chips.push_back(format("%d × %d", width, height));
        } else if (!asBytes && (isUtf16(data) || readsAsText(data.data(), std::min(data.size(), TextLooked)))) {
            if (isUtf16(data))
                data = fromUtf16(data);
            kind = "Text";
            chips[0] = kind;
            lines = linesOf(data.data(), data.size(), total);
            std::string count = withCommas(total) + (total == 1 ? " line" : " lines");
            chips.push_back((i64)want < node.size ? count + " 的前 " + formatSize((i64)want) : count);
        } else {
            // Any other kind, as yet: its bytes as they are, which is how every kind can be looked at.
            raw.assign(data.begin(), data.begin() + (ptrdiff_t)std::min(data.size(), BytesShown));
            rawShown = true;
            chips.push_back((i64)raw.size() < node.size ? "前 " + formatSize((i64)raw.size()) + " 按字节" : "按字节");
        }
    }
    sys::Lock lock(_mutex);
    // Moved on from, or asked to be shown another way, since.
    if (_inside.get() != &inside || _insideWant != index || inside.asBytes != asBytes)
        return;
    inside.previewOf = index;
    inside.previewReady = true;
    inside.previewKind = kind;
    inside.chips = std::move(chips);
    inside.lines = std::move(lines);
    inside.totalLines = total;
    inside.raw = std::move(raw);
    inside.rawShown = rawShown;
    inside.picture = picture;
    inside.pictureFull = pictureFull;
    inside.breakdown = std::move(breakdown);
    // What it turned out to be, on the list too, once it's been looked at: text with no extension, or data named as a picture.
    if (!node.folder && kind != "Unreadable")
        inside.nodes[(size_t)index].kind = kind;
    inside.fullTop = 0;
    _dirty = true;
}

/// A file, or all of a folder, copied to Files from games, in a folder named after the game, the way it is inside it: never where extracted
/// games go, as a few of a game's parts there could be taken for all of it, or end up mixed in with an extraction of it.
void App::saveInside(int index) {
    Inside& inside = *_inside;
    std::vector<int> files;
    i64 bytes = 0;
    std::vector<int> stack{index};
    while (!stack.empty()) {
        int at = stack.back();
        stack.pop_back();
        const InsideNode& node = inside.nodes[(size_t)at];
        if (node.folder) {
            for (auto it = node.children.rbegin(); it != node.children.rend(); ++it)
                stack.push_back(*it);
        } else {
            files.push_back(at);
            bytes += node.size;
        }
    }
    auto pathOf = [&](int at) {
        std::string path;
        for (int i = at; i > 0; i = inside.nodes[(size_t)i].parent)
            path = "/" + LocalNames::forFileSystem(inside.nodes[(size_t)i].name) + path;
        return path;
    };
    std::string root = joinPath(filesFromGamesFolder(), LocalNames::forFileSystem(inside.stem.empty() ? inside.name : inside.stem));
    {
        sys::Lock lock(_mutex);
        inside.saving = true;
        inside.savedFiles = 0;
        inside.savingFiles = (i64)files.size();
        inside.savedBytes = 0;
        inside.savingBytes = bytes;
        _dirty = true;
    }
    Bytes buffer(4 << 20);
    for (int at : files) {
        if (_insideSaveStop)
            throw Cancelled();
        const InsideNode& node = inside.nodes[(size_t)at];
        std::string path = root + pathOf(at);
        sys::makeDirectories(path.substr(0, path.rfind('/')));
        bool whole = false;
        try {
            sys::File out(path, sys::File::Mode::Create);
            for (i64 done = 0; done < node.size;) {
                if (_insideSaveStop)
                    throw Cancelled();
                size_t n = (size_t)std::min<i64>((i64)buffer.size(), node.size - done);
                inside.parts[(size_t)node.part].second->read(node.offset + done, buffer.data(), n);
                out.write(done, buffer.data(), n);
                done += (i64)n;
                sys::Lock lock(_mutex);
                inside.savedBytes += (i64)n;
                _dirty = true;
            }
            out.flush();
            whole = true;
        } catch (...) {
            // A file cut short is no use to anyone, and would look like the whole of it.
            if (!whole)
                sys::removeFile(path);
            throw;
        }
        sys::Lock lock(_mutex);
        inside.savedFiles++;
        _dirty = true;
    }
    std::string where = root + (index == 0 ? "" : pathOf(index));
    logLine(format("已保存 %d 个文件：%s → %s", (int)files.size(), inside.name.c_str(), where.c_str()));
    sys::Lock lock(_mutex);
    inside.saving = false;
    inside.savedTo = where;
    _toast = "Saved to " + where;
    _toastUntil = _time + 5;
    _dirty = true;
}

/// Files and folders whose names have what's typed in them, from anywhere in the game, shown as a folder of their own.
void App::findInside() {
    if (!_inside || !_inside->ready || !_inside->problem.empty())
        return;
    std::optional<std::string> typed = _platform.typeText("按名称查找文件或文件夹", 64);
    if (!typed || typed->empty())
        return;
    std::string wanted = lowered(*typed);
    sys::Lock lock(_mutex);
    Inside& inside = *_inside;
    std::vector<int> found;
    for (size_t i = 1; i < inside.nodes.size() && found.size() < 2000; i++) {
        if (lowered(inside.nodes[i].name).find(wanted) != std::string::npos)
            found.push_back((int)i);
    }
    if (found.empty()) {
        _toast = "其中没有匹配的名称";
        _toastUntil = _time + 3;
        _dirty = true;
        return;
    }
    if (inside.foundFrom < 0)
        inside.foundFrom = inside.focus;
    inside.found = std::move(found);
    inside.finding = *typed;
    inside.focus = 0;
    inside.lastFocus = -1;
    inside.scroll = 0;
    _glideList = -1;
    _insideWant = inside.found[0];
    _insideWake.wakeAll();
    _dirty = true;
}

/// X on the details: what X does on the list, to this game alone, when there's anything for it to do.
std::string App::detailsActionLocked() const {
    if (!_inside)
        return "";
    for (const Row& row : _rows) {
        if (row.key != _inside->key || row.tab != _inside->tab)
            continue;
        if (!row.blocked.empty() || row.state == RowState::Working)
            return "";
        bool toInstall = row.tab == TabFiles && installsGameFiles();
        if (isCompressed(row) && !toInstall)
            return "";
        return actionWordOn(row.tab);
    }
    return "";
}

// -------------------------------------------------------------------------------------------------------------------
// The buttons

void App::toggleInsideBytesLocked() {
    if (!_inside || !_inside->ready)
        return;
    _inside->asBytes = !_inside->asBytes;
    _inside->previewOf = -1;
    _inside->previewReady = false;
    _insideWake.wakeAll();
    _dirty = true;
}

void App::saveInsideFocusedLocked() {
    if (!_inside || !_inside->ready || !_inside->problem.empty() || _inside->saving)
        return;
    const std::vector<int>& shown = insideShownLocked();
    if (shown.empty())
        return;
    Inside& inside = *_inside;
    _insideSave = shown[(size_t)std::min(inside.focus, (int)shown.size() - 1)];
    _insideSaveStop = false;
    inside.saving = true;
    inside.savedFiles = inside.savingFiles = inside.savedBytes = inside.savingBytes = 0;
    _insideWake.wakeAll();
    _dirty = true;
}

void App::insideToTopLocked() {
    if (!_inside)
        return;
    Inside& inside = *_inside;
    inside.folder = 0;
    inside.found.clear();
    inside.foundFrom = -1;
    inside.focusAbove.clear();
    inside.focus = 0;
    inside.scroll = 0;
    inside.lastFocus = -1;
    _glideList = -1;
    const std::vector<int>& shown = insideShownLocked();
    _insideWant = shown.empty() ? -1 : shown[0];
    _insideWake.wakeAll();
    _dirty = true;
}

void App::goInside() {
    sys::Lock lock(_mutex);
    if (!_inside || !_inside->ready || !_inside->problem.empty())
        return;
    _inside->fromDetails = true;
    _screen = Screen::Inside;
    _glideList = -1;
    _dirty = true;
}

void App::updateDetails(u32 pressed, const InputState& input, double dt) {
    if (!_inside)
        return;
    {
        sys::Lock lock(_mutex);
        Inside& inside = *_inside;
        // The facts go up and down when there are more than fit.
        if (repeated(input.held, ButtonDown, dt) && inside.detailsMost > 0)
            inside.detailsTop = std::min(inside.detailsTop + 48, inside.detailsMost);
        if (repeated(input.held, ButtonUp, dt))
            inside.detailsTop = std::max(inside.detailsTop - 48, 0);
    }
    if (pressed & ButtonB) {
        closeInside();
    } else if (pressed & ButtonPlus) {
        openMenu(MenuInside);
    } else if (pressed & ButtonA) {
        goInside();
    } else if (pressed & ButtonX) {
        bool can;
        {
            sys::Lock lock(_mutex);
            can = !detailsActionLocked().empty();
        }
        if (can) {
            closeInside();
            onlyTheSelected([this] { pressCompress(); });
        }
    }
}

void App::updateInside(u32 pressed, const InputState& input, double dt) {
    if (!_inside)
        return;
    InsideAfter after;
    {
        sys::Lock lock(_mutex);
        after = updateInsideLocked(pressed, input, dt);
    }
    if (after == InsideAfter::Close)
        closeInside();
    else if (after == InsideAfter::Find)
        findInside();
    else if (after == InsideAfter::Menu)
        openMenu(MenuInside);
}

App::InsideAfter App::updateInsideLocked(u32 pressed, const InputState& input, double dt) {
    Inside& inside = *_inside;
    if (inside.full) {
        int most = std::max(inside.fullLines() - InsideFullLines, 0);
        if (repeated(input.held, ButtonDown, dt))
            inside.fullTop = std::min(inside.fullTop + 1, most);
        if (repeated(input.held, ButtonUp, dt))
            inside.fullTop = std::max(inside.fullTop - 1, 0);
        if (repeated(input.held, ButtonRight, dt) || repeated(input.held, ButtonR, dt))
            inside.fullTop = std::min(inside.fullTop + InsideFullLines, most);
        if (repeated(input.held, ButtonLeft, dt) || repeated(input.held, ButtonL, dt))
            inside.fullTop = std::max(inside.fullTop - InsideFullLines, 0);
        if (pressed & (ButtonB | ButtonA))
            inside.full = false;
        _dirty = true;
        return InsideAfter::Nothing;
    }
    if (inside.saving) {
        // B stops saving, and that's all there is to do while it saves.
        if (pressed & ButtonB)
            _insideSaveStop = true;
        return InsideAfter::Nothing;
    }
    if (!inside.ready || !inside.problem.empty()) {
        // While it's opening, B stops it, wherever it was opened from.
        if (pressed & ButtonB) {
            if (!inside.ready || !inside.fromDetails)
                return InsideAfter::Close;
            _screen = Screen::Details;
            _dirty = true;
        }
        return InsideAfter::Nothing;
    }
    const std::vector<int>& shown = insideShownLocked();
    int count = (int)shown.size();
    constexpr int Page = (InsideListBottom - InsideListTop) / InsideItemH - 1;
    if (repeated(input.held, ButtonDown, dt) && inside.focus + 1 < count)
        inside.focus++;
    if (repeated(input.held, ButtonUp, dt) && inside.focus > 0)
        inside.focus--;
    if (repeated(input.held, ButtonRight, dt) || repeated(input.held, ButtonR, dt))
        inside.focus = std::min(inside.focus + Page, std::max(count - 1, 0));
    if (repeated(input.held, ButtonLeft, dt) || repeated(input.held, ButtonL, dt))
        inside.focus = std::max(inside.focus - Page, 0);
    int focused = count > 0 ? shown[(size_t)std::min(inside.focus, count - 1)] : -1;
    if ((pressed & ButtonA) && focused >= 0) {
        const InsideNode& node = inside.nodes[(size_t)focused];
        if (node.folder) {
            // Into it, from wherever it was found too, sliding in from the right.
            inside.slideSince = _time;
            inside.slideFrom = 1;
            inside.focusAbove.push_back(inside.foundFrom >= 0 ? inside.foundFrom : inside.focus);
            inside.folder = focused;
            inside.found.clear();
            inside.foundFrom = -1;
            inside.focus = 0;
            inside.scroll = 0;
            inside.lastFocus = -1;
            _glideList = -1;
        } else if (inside.previewOf == focused && inside.previewReady && (!inside.lines.empty() || inside.rawShown || inside.pictureFull)) {
            inside.full = true;
            inside.fullTop = 0;
        }
    } else if (pressed & ButtonB) {
        if (inside.foundFrom >= 0) {
            inside.focus = inside.foundFrom;
            inside.foundFrom = -1;
            inside.found.clear();
            inside.lastFocus = -1;
            _glideList = -1;
        } else if (inside.folder != 0) {
            inside.slideSince = _time;
            inside.slideFrom = -1;
            int from = inside.folder;
            inside.folder = inside.nodes[(size_t)from].parent;
            inside.focus = inside.focusAbove.empty() ? 0 : inside.focusAbove.back();
            if (!inside.focusAbove.empty())
                inside.focusAbove.pop_back();
            inside.lastFocus = -1;
            _glideList = -1;
        } else if (inside.fromDetails) {
            _screen = Screen::Details;
        } else {
            return InsideAfter::Close;
        }
    } else if ((pressed & ButtonX) && focused >= 0) {
        saveInsideFocusedLocked();
    } else if (pressed & ButtonY) {
        return InsideAfter::Find;
    } else if (pressed & ButtonPlus) {
        return InsideAfter::Menu;
    } else if (pressed & ButtonMinus) {
        toggleInsideBytesLocked();
    }
    const std::vector<int>& now = insideShownLocked();
    if (!now.empty()) {
        inside.focus = std::min(inside.focus, (int)now.size() - 1);
        int want = now[(size_t)inside.focus];
        if (want != _insideWant || inside.previewOf != want) {
            if (want != _insideWant)
                inside.wantSince = _time;
            _insideWant = want;
            _insideWake.wakeAll();
        }
    }
    _dirty = true;
    return InsideAfter::Nothing;
}

}  // namespace hbp
