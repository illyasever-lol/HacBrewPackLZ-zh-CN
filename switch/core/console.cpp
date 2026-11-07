#include "console.h"

#include <algorithm>
#include <map>

namespace hbp {

const char* storageName(Storage storage) {
    switch (storage) {
        case Storage::Sd:
            return "SD 卡";
        case Storage::Nand:
            return "主机";
        case Storage::GameCard:
            return "游戏卡带";
        default:
            return "未知存储位置";
    }
}

bool ContentId::parse(const std::string& hex, ContentId& out) {
    Bytes bytes;
    if (!fromHex(hex, bytes) || bytes.size() != 16)
        return false;
    memcpy(out.bytes, bytes.data(), 16);
    return true;
}

i64 InstalledGame::totalBytes() const {
    i64 total = 0;
    for (auto& meta : metas) {
        for (auto& content : meta.contents)
            total += content.size;
    }
    return total;
}

InstalledArchive::InstalledArchive(ConsoleApi& console, const InstalledGame& game) : _console(console) {
    _path = game.name;
    for (auto& meta : game.metas) {
        for (auto& content : meta.contents) {
            std::string name = content.id.hex() + (content.type == Cnmt::ContentTypeMeta ? ".cnmt.nca" : ".nca");
            if (find(name))
                continue;
            _entries.push_back(Entry{name, (i64)_where.size(), content.size});
            _where.emplace_back(meta.storage, content.id);
            _length += content.size;
        }
    }
}

SourcePtr InstalledArchive::open(const Entry& entry) {
    auto& where = _where[(size_t)entry.offset];
    return _console.openContent(where.first, where.second, entry.size);
}

Bytes installMetaFrom(const ContentMeta& rebuilt, const NcaInfo& metaNca) {
    // In the database a content meta has a short header of its own, lists itself as a content ahead of the others, and leaves out
    // the delta fragments, which only go with an update on its way to being installed.
    constexpr size_t InfoSize = 0x18;
    size_t count = 0;
    for (auto& record : rebuilt.records)
        count += record.type != Cnmt::ContentTypeDeltaFragment ? 1 : 0;
    Bytes out(8 + (size_t)rebuilt.extendedHeaderSize + (count + 1) * InfoSize);
    u8* o = out.data();
    wr16(o, (u16)rebuilt.extendedHeaderSize);
    wr16(o + 2, (u16)(count + 1));
    wr16(o + 4, 0);
    o[6] = rebuilt.raw[0x14];
    o[7] = 0;
    memcpy(o + 8, &rebuilt.raw[0x20], (size_t)rebuilt.extendedHeaderSize);

    auto writeInfo = [](u8* info, const u8* id, i64 size, u8 type, u8 idOffset) {
        memcpy(info, id, 16);
        wr32(info + 0x10, (u32)size);
        info[0x14] = (u8)(size >> 32);
        info[0x15] = 0;
        info[0x16] = type;
        info[0x17] = idOffset;
    };
    u8* info = o + 8 + rebuilt.extendedHeaderSize;
    writeInfo(info, metaNca.hash, metaNca.size, Cnmt::ContentTypeMeta, 0);
    info += InfoSize;
    const u8* records = rebuilt.raw.data() + 0x20 + rebuilt.extendedHeaderSize;
    for (size_t i = 0; i < rebuilt.records.size(); i++) {
        if (rebuilt.records[i].type == Cnmt::ContentTypeDeltaFragment)
            continue;
        const u8* r = records + i * 0x38;
        writeInfo(info, r + 0x20, rebuilt.records[i].size, r[0x36], r[0x37]);
        info += InfoSize;
    }
    return out;
}

// -------------------------------------------------------------------------------------------------------------------
// The journal: a plain text file, replaced whole, that says how far a replacement has got.

namespace {

struct Journal {
    enum class Phase { Writing, Switching };

    Phase phase = Phase::Writing;
    u64 applicationId = 0;
    Storage storage = Storage::None;
    std::vector<std::string> placeholders;
    std::vector<InstalledContent> newContents;
    /// Contents that are needed afterwards though they were there already: another title of the game's, or one rebuilt to the same bytes.
    std::vector<ContentId> kept;
    std::vector<InstalledMeta> oldMetas;
    MetaKey newKey;
    Bytes installMeta;
    std::vector<std::pair<MetaKey, Storage>> newRecords;

    std::string text() const {
        std::string t = format("phase %d\napp %016llx\nstorage %d\n", (int)phase, (unsigned long long)applicationId, (int)storage);
        for (auto& p : placeholders)
            t += "placeholder " + p + "\n";
        for (auto& c : newContents)
            t += format("new %s %lld %d %d\n", c.id.hex().c_str(), (long long)c.size, c.type, c.idOffset);
        for (auto& id : kept)
            t += "kept " + id.hex() + "\n";
        for (auto& m : oldMetas) {
            t += format("oldmeta %016llx %u %d %d %d\n", (unsigned long long)m.key.id, m.key.version, m.key.type, m.key.installType, (int)m.storage);
            for (auto& c : m.contents)
                t += format("oldcontent %s\n", c.id.hex().c_str());
        }
        if (phase == Phase::Switching) {
            t += format("newkey %016llx %u %d %d\n", (unsigned long long)newKey.id, newKey.version, newKey.type, newKey.installType);
            t += "installmeta " + toHex(installMeta.data(), installMeta.size()) + "\n";
            for (auto& r : newRecords)
                t += format("record %016llx %u %d %d %d\n", (unsigned long long)r.first.id, r.first.version, r.first.type, r.first.installType, (int)r.second);
        }
        u8 hash[32];
        sha256(t.data(), t.size(), hash);
        return t + "check " + toHex(hash, 32) + "\n";
    }

    static bool parse(const std::string& whole, Journal& out) {
        size_t check = whole.rfind("check ");
        if (check == std::string::npos)
            return false;
        u8 hash[32];
        sha256(whole.data(), check, hash);
        if (whole.compare(check + 6, 64, toHex(hash, 32)) != 0)
            return false;

        size_t position = 0;
        while (position < check) {
            size_t end = whole.find('\n', position);
            std::string line = whole.substr(position, end - position);
            position = end + 1;
            size_t space = line.find(' ');
            std::string word = line.substr(0, space);
            std::string rest = space == std::string::npos ? "" : line.substr(space + 1);
            unsigned long long id = 0;
            unsigned version = 0;
            int type = 0, install = 0, storage = 0;
            char hex[65] = {};
            long long size = 0;
            int idOffset = 0;
            if (word == "phase") {
                out.phase = (Phase)atoi(rest.c_str());
            } else if (word == "app") {
                out.applicationId = strtoull(rest.c_str(), nullptr, 16);
            } else if (word == "storage") {
                out.storage = (Storage)atoi(rest.c_str());
            } else if (word == "placeholder") {
                out.placeholders.push_back(rest);
            } else if (word == "new" && sscanf(rest.c_str(), "%32s %lld %d %d", hex, &size, &type, &idOffset) == 4) {
                InstalledContent c;
                if (!ContentId::parse(hex, c.id))
                    return false;
                c.size = size;
                c.type = (u8)type;
                c.idOffset = (u8)idOffset;
                out.newContents.push_back(c);
            } else if (word == "kept") {
                ContentId id;
                if (!ContentId::parse(rest, id))
                    return false;
                out.kept.push_back(id);
            } else if (word == "oldmeta" && sscanf(rest.c_str(), "%llx %u %d %d %d", &id, &version, &type, &install, &storage) == 5) {
                InstalledMeta m;
                m.key.id = id;
                m.key.version = version;
                m.key.type = (u8)type;
                m.key.installType = (u8)install;
                m.storage = (Storage)storage;
                out.oldMetas.push_back(m);
            } else if (word == "oldcontent" && !out.oldMetas.empty()) {
                InstalledContent c;
                if (!ContentId::parse(rest, c.id))
                    return false;
                out.oldMetas.back().contents.push_back(c);
            } else if (word == "newkey" && sscanf(rest.c_str(), "%llx %u %d %d", &id, &version, &type, &install) == 4) {
                out.newKey.id = id;
                out.newKey.version = version;
                out.newKey.type = (u8)type;
                out.newKey.installType = (u8)install;
            } else if (word == "installmeta") {
                if (!fromHex(rest, out.installMeta))
                    return false;
            } else if (word == "record" && sscanf(rest.c_str(), "%llx %u %d %d %d", &id, &version, &type, &install, &storage) == 5) {
                MetaKey key;
                key.id = id;
                key.version = version;
                key.type = (u8)type;
                key.installType = (u8)install;
                out.newRecords.emplace_back(key, (Storage)storage);
            }
        }
        return true;
    }
};

std::string journalPath(const std::string& folder) { return folder + "/journal.txt"; }
std::string journalTempPath(const std::string& folder) { return folder + "/journal.new"; }

/// The new one is written beside the old, so there's always a whole one to find, whenever the power goes.
void saveJournal(const std::string& folder, const Journal& journal) {
    sys::makeDirectories(folder);
    std::string text = journal.text();
    std::string temp = journalTempPath(folder);
    {
        sys::File file(temp, sys::File::Mode::Create);
        file.write(0, text.data(), text.size());
        file.flush();
    }
    sys::commit(temp);
    sys::removeFile(journalPath(folder));
    sys::renameFile(temp, journalPath(folder));
    sys::commit(journalPath(folder));
}

void clearJournal(const std::string& folder) {
    sys::removeFile(journalPath(folder));
    sys::removeFile(journalTempPath(folder));
    sys::commit(journalPath(folder));
}

bool loadJournalFile(const std::string& path, Journal& journal) {
    if (!sys::exists(path))
        return false;
    sys::File file(path, sys::File::Mode::Read);
    std::string text((size_t)file.size(), '\0');
    if (!text.empty())
        file.read(0, &text[0], text.size());
    return Journal::parse(text, journal);
}

/// Everything from the switch on. Each step can safely be done again, so a power cut part way through just means starting here again.
void finishSwitch(ConsoleApi& console, const Journal& journal) {
    console.setMeta(journal.storage, journal.newKey, journal.installMeta);
    console.setRecords(journal.applicationId, journal.newRecords);

    for (auto& old : journal.oldMetas) {
        if (!(old.key == journal.newKey) && console.hasMeta(old.storage, old.key))
            console.removeMeta(old.storage, old.key);
    }
    for (auto& old : journal.oldMetas) {
        for (auto& content : old.contents) {
            bool stillUsed = std::any_of(journal.newContents.begin(), journal.newContents.end(), [&](const InstalledContent& c) { return c.id == content.id && old.storage == journal.storage; });
            stillUsed = stillUsed || std::any_of(journal.kept.begin(), journal.kept.end(), [&](const ContentId& id) { return id == content.id && old.storage == journal.storage; });
            if (!stillUsed && console.hasContent(old.storage, content.id))
                console.deleteContent(old.storage, content.id);
        }
    }
}

/// Takes away whatever had been written. The game is still as it was.
void undoWriting(ConsoleApi& console, const Journal& journal) {
    for (auto& placeholder : journal.placeholders)
        console.deletePlaceholder(journal.storage, placeholder);
    for (auto& content : journal.newContents) {
        // An old title rebuilt to the very same bytes shares its content: that stays.
        bool wasThere = false;
        for (auto& old : journal.oldMetas) {
            for (auto& c : old.contents)
                wasThere = wasThere || (c.id == content.id && old.storage == journal.storage);
        }
        if (!wasThere && console.hasContent(journal.storage, content.id))
            console.deleteContent(journal.storage, content.id);
    }
}

/// The game data of an NCA, as this program's own code reads it.
class OwnGameData : public FirmwareView {
public:
    OwnGameData(SourcePtr raw, const std::string& name, const Keyset& keyset) : _tickets(keyset), _nca(std::move(raw), name, keyset, _tickets) {
        for (int i = 0; i < NcaFile::SectionCount && !_image; i++) {
            if (_nca.section(i) && _nca.section(i)->fsType() == NcaFsSection::FsRomFs)
                _image = _nca.openRomFsImage(*_nca.section(i), nullptr);
        }
        if (!_image)
            throw ToolError(name + "没有游戏数据");
        RomFsArchive archive = RomFsArchive::open(_image, name);
        for (auto& file : archive.files)
            _files["/" + file.path] = file;
    }

    void read(const std::string& path, i64 offset, void* buffer, size_t size) override {
        auto found = _files.find(path);
        if (found == _files.end())
            throw ToolError("游戏数据中没有此文件：" + path);
        if (offset + (i64)size > found->second.size)
            throw ToolError("读取超出游戏数据中的文件末尾");
        _image->read(found->second.offset + offset, buffer, size);
    }

private:
    TicketStore _tickets;
    NcaFile _nca;
    SourcePtr _image;
    std::map<std::string, RomFsArchive::Entry> _files;
};

/// The logo of a Program NCA, which is its third section.
class OwnLogo : public FirmwareView {
public:
    OwnLogo(SourcePtr raw, const std::string& name, const Keyset& keyset) : _tickets(keyset), _nca(std::move(raw), name, keyset, _tickets) {
        constexpr int LogoSection = 2;
        if (!_nca.section(LogoSection) || _nca.section(LogoSection)->fsType() != NcaFsSection::FsPartition)
            throw ToolError(name + "没有启动画面");
        _archive = _nca.openPfs0(*_nca.section(LogoSection), nullptr);
    }

    void read(const std::string& path, i64 offset, void* buffer, size_t size) override {
        for (auto& file : _archive->files()) {
            if ("/" + file.name != path)
                continue;
            if (offset + (i64)size > file.size)
                throw ToolError("读取超出" + path);
            _archive->open(file)->read(offset, buffer, size);
            return;
        }
        throw ToolError("启动画面中没有此文件：" + path);
    }

private:
    TicketStore _tickets;
    NcaFile _nca;
    std::unique_ptr<Pfs0Archive> _archive;
};

/// Puts each NCA into the console's storage as it comes, keeping the journal up to date.
class InstallOutput : public TitleOutput, private NcaStream {
public:
    InstallOutput(ConsoleApi& console, Journal& journal, std::string journalFolder, const char* readBackAs = "正在逐字节校验新副本") : _console(console), _journal(journal), _journalFolder(std::move(journalFolder)), _readBackAs(readBackAs) {}

    NcaStream& beginNca(const char*, i64 size, const char*) override {
        i64 free = _console.freeBytes(_journal.storage);
        if (free < size)
            throw ToolError(format("There isn't enough room on %s: this needs %s more, and %s is free", storageName(_journal.storage), formatSize(size).c_str(), formatSize(free).c_str()));
        _writer = _console.createContent(_journal.storage, size);
        _journal.placeholders.push_back(_writer->token());
        saveJournal(_journalFolder, _journal);
        _position = 0;
        _growing = size < 0;
        _held = nullptr;
        return *this;
    }

    void endNca(const NcaInfo& info, const char* extension) override {
        ContentId id;
        memcpy(id.bytes, info.hash, 16);
        bool readBack = _held != nullptr;
        if (_console.hasContent(_journal.storage, id)) {
            // Rebuilt to the very same bytes as something that's there, which is what's used from now on.
            _journal.kept.push_back(id);
            saveJournal(_journalFolder, _journal);
            if (readBack)
                _console.deleteContent(_journal.storage, _heldId);
        } else {
            InstalledContent content;
            content.id = id;
            content.size = info.size;
            content.type = strcmp(extension, ".cnmt.nca") == 0 ? Cnmt::ContentTypeMeta : 0;
            // Told first, so that a power cut between here and there leaves nothing that can't be found again.
            _journal.newContents.push_back(content);
            saveJournal(_journalFolder, _journal);
            // If all of it was kept in memory it never got read back, and it's still whatever size it had grown to.
            if (_growing && !readBack)
                _writer->setSize(_position);
            _writer->finishAs(id);
            if (!readBack)
                written.push_back(content);
        }
        if (readBack) {
            _held = nullptr;
            _journal.newContents.erase(std::remove_if(_journal.newContents.begin(), _journal.newContents.end(), [&](const InstalledContent& c) { return c.id == _heldId; }), _journal.newContents.end());
        }
        _journal.placeholders.pop_back();
        _writer.reset();
        hashes.emplace_back(id, Bytes(info.hash, info.hash + 32));
        totalBytes += info.size;
        saveJournal(_journalFolder, _journal);
    }

    void finish(const ContentMeta& rebuilt, const NcaInfo& metaNca) override {
        installMeta = installMetaFrom(rebuilt, metaNca);
        key.id = rebuilt.titleId;
        key.version = rebuilt.version;
        key.type = rebuilt.type;
        key.installType = 0;
    }

    Bytes installMeta;
    MetaKey key;
    std::vector<std::pair<ContentId, Bytes>> hashes;
    /// What's been written without having been read back along the way.
    std::vector<InstalledContent> written;
    i64 totalBytes = 0;

private:
    void write(const void* data, size_t size) override {
        _writer->write(_position, data, size);
        _position += (i64)size;
    }

    void overwrite(i64 offset, const void* data, size_t size) override { _writer->write(offset, data, size); }

    void readBack(i64 offset, void* data, size_t size) override {
        if (!_held) {
            _writer->setSize(_position);
            sys::randomBytes(_heldId.bytes, sizeof(_heldId.bytes));
            InstalledContent content;
            content.id = _heldId;
            content.size = _position;
            _journal.newContents.push_back(content);
            saveJournal(_journalFolder, _journal);
            _writer->holdAs(_heldId);
            _held = _console.openContent(_journal.storage, _heldId, _position);
        }
        _held->read(offset, data, size);
    }

    const char* readBackLabel() const override { return _readBackAs; }

    ConsoleApi& _console;
    Journal& _journal;
    std::string _journalFolder;
    const char* _readBackAs;
    std::unique_ptr<ContentWriter> _writer;
    i64 _position = 0;
    bool _growing = false;
    ContentId _heldId;
    SourcePtr _held;
};

/// Every byte of what was written, against the hash it was given. The name of an NCA is the start of its hash.
void checkWritten(ConsoleApi& console, Storage storage, const std::vector<InstalledContent>& contents, const std::vector<std::pair<ContentId, Bytes>>& hashes) {
    i64 total = 0;
    for (auto& c : contents)
        total += c.size;
    Progress progress("正在逐字节校验新副本", total);
    Bytes buffer(1 << 20);
    i64 done = 0;
    for (auto& content : contents) {
        SourcePtr source = console.openContent(storage, content.id, content.size);
        Sha256 hash;
        for (i64 position = 0; position < content.size;) {
            size_t n = (size_t)std::min<i64>((i64)buffer.size(), content.size - position);
            source->read(position, buffer.data(), n);
            hash.update(buffer.data(), n);
            position += (i64)n;
            progress.report(done + position);
        }
        done += content.size;
        u8 result[32];
        hash.finish(result);
        auto expected = std::find_if(hashes.begin(), hashes.end(), [&](auto& h) { return h.first == content.id; });
        if (expected == hashes.end() || memcmp(result, expected->second.data(), 32) != 0)
            throw ToolError(format("What was written to %s doesn't read back the same (%s). The card may be failing", storageName(storage), content.id.hex().c_str()));
    }
    progress.done();
}

/// A spread of files read through a new copy, against the same files in the original.
void compareGameData(FirmwareView& view, const SourcePtr& image, const std::string& what, const char* label) {
    constexpr int FilesToCheck = 24;
    constexpr size_t PieceSize = 1 << 16;

    RomFsArchive archive = RomFsArchive::open(image, what);
    std::vector<RomFsArchive::Entry> files;
    for (auto& file : archive.files) {
        if (file.size > 0)
            files.push_back(file);
    }
    if (files.empty())
        return;
    std::stable_sort(files.begin(), files.end(), [](auto& a, auto& b) { return a.offset < b.offset; });

    Progress progress(label, FilesToCheck);
    Bytes mine(PieceSize), theirs(PieceSize);
    size_t step = std::max<size_t>(files.size() / FilesToCheck, 1);
    int checked = 0;
    for (size_t i = 0; i < files.size(); i += step) {
        const auto& file = files[i];
        for (i64 at : {(i64)0, std::max<i64>(file.size / 2, 0), std::max<i64>(file.size - (i64)PieceSize, 0)}) {
            size_t n = (size_t)std::min<i64>((i64)PieceSize, file.size - at);
            image->read(file.offset + at, mine.data(), n);
            view.read("/" + file.path, at, theirs.data(), n);
            if (memcmp(mine.data(), theirs.data(), n) != 0)
                throw ToolError(format("The new copy reads %s differently from the original, so the original stays", file.path.c_str()));
        }
        progress.report(++checked);
    }
    progress.done();
    sink().message(format("Read %d files of %s back, and they're the same", checked, what.c_str()));
}

bool romFsSectionOf(const ContentPair& content, SectionSource& source) {
    for (int i = 0; i < NcaFile::SectionCount; i++) {
        if (content.sectionAt(i, source) && source.section->fsType() == NcaFsSection::FsRomFs)
            return true;
    }
    return false;
}

constexpr const char* FirmwareRefusal = "This console's firmware won't read compressed games, so nothing was changed. It needs firmware 12.0.0 or newer and sigpatches. ";

/// The ID the test copy goes by. It has nothing to do with any game, so that no game can get in the way of it.
constexpr u64 TestCopyId = 0x01FFFFFFFFFFF000ull;

/// Something that compresses and something that doesn't, each with something in it no other test copy has.
std::map<std::string, Bytes> testCopyFiles() {
    Bytes compressible(1 << 18);
    for (size_t i = 0; i < compressible.size(); i++)
        compressible[i] = (u8)((i / 64) * 37);
    Bytes random(1 << 17);
    sys::randomBytes(random.data(), random.size());
    memcpy(compressible.data(), random.data(), 64);
    return {{"compressible.bin", std::move(compressible)}, {"random.bin", std::move(random)}};
}

/// The logo says whether the firmware takes the new Program NCA, as it's the one part of it the firmware can be asked to mount. Not being
/// able to doesn't mean anything is wrong, as not every game has one and the small test has already been passed, but different bytes would.
void checkLogo(ConsoleApi& console, Storage storage, const ContentId& id, const SourcePtr& raw, u64 programId, const Keyset& keyset) {
    std::unique_ptr<FirmwareView> mine, theirs;
    try {
        mine = readLogoOf(raw, id.hex(), keyset);
        theirs = console.mountLogo(storage, id, programId);
    } catch (const ToolError& e) {
        sink().message(std::string("未向固件查询新副本的启动画面：") + e.what());
        return;
    }
    static const char* const files[] = {"/NintendoLogo.png", "/StartupMovie.gif"};
    Bytes a(1 << 16), b(1 << 16);
    for (const char* file : files) {
        try {
            mine->read(file, 0, a.data(), 0x400);
            theirs->read(file, 0, b.data(), 0x400);
        } catch (const ToolError& e) {
            sink().message(format("The firmware wasn't asked about %s: %s", file, e.what()));
            continue;
        }
        if (memcmp(a.data(), b.data(), 0x400) != 0)
            throw ToolError(format("The firmware reads %s differently from what was written, so the original stays", file));
    }
    sink().message("固件已接受新副本");
}

/// The firmware reads the data it can be asked to, and the rest gets read back by this program. Every byte of it is already known to be
/// what was written, every block was decoded and compared when it was compressed, and the small test shows the firmware can read the
/// format, so what's left to catch is game data that doesn't come back the same.
void checkNewCopy(ConsoleApi& console, const TitleJob& job, const Journal& journal, const Keyset& keyset) {
    for (auto& content : job.contents) {
        SectionSource source;
        if ((content.type != Cnmt::ContentTypeProgram && content.type != Cnmt::ContentTypeData) || !romFsSectionOf(content, source))
            continue;
        NcaFile& original = content.primary();

        bool found = false;
        for (auto& candidate : journal.newContents) {
            SourcePtr raw = console.openContent(journal.storage, candidate.id, candidate.size);
            TicketStore none(keyset);
            NcaFile rebuilt(raw, candidate.id.hex() + ".nca", keyset, none);
            if (rebuilt.contentType() != original.contentType() || rebuilt.contentIndex() != original.contentIndex())
                continue;
            found = true;

            SourcePtr image = source.owner->openRomFsImage(*source.section, source.original);
            if (content.type == Cnmt::ContentTypeData) {
                std::unique_ptr<FirmwareView> view;
                try {
                    view = console.mountGameData(journal.storage, candidate.id, rebuilt.programId());
                } catch (const ToolError& e) {
                    throw ToolError(FirmwareRefusal + std::string(e.what()));
                }
                compareGameData(*view, image, candidate.id.hex(), "让固件回读");
            } else {
                checkLogo(console, journal.storage, candidate.id, raw, rebuilt.programId(), keyset);
                auto view = readGameDataOf(raw, candidate.id.hex(), keyset);
                compareGameData(*view, image, candidate.id.hex(), "Reading the new copy back");
            }
        }
        if (!found && !journal.kept.empty())
            sink().message("新副本有部分与原内容相同，因此未回读该部分");
    }
}

/// The same for what was made out of an extracted game, against the files in its folder.
void checkBuiltCopy(ConsoleApi& console, const ExtractedGame& game, const Journal& journal, const Keyset& keyset) {
    if (!game.hasRomFs)
        return;
    SourcePtr image = game.romFsImage;
    for (auto& candidate : journal.newContents) {
        SourcePtr raw = console.openContent(journal.storage, candidate.id, candidate.size);
        TicketStore none(keyset);
        NcaFile rebuilt(raw, candidate.id.hex() + ".nca", keyset, none);
        if (rebuilt.contentType() != NcaContentType::Program)
            continue;
        checkLogo(console, journal.storage, candidate.id, raw, rebuilt.programId(), keyset);
        auto view = readGameDataOf(raw, candidate.id.hex(), keyset);
        compareGameData(*view, image, candidate.id.hex(), "Reading the new copy back");
    }
}

/// What goes into the title is up to whoever calls this, and so is what to compare it with afterwards.
template <typename Write, typename Check, typename Release>
ReplaceResult replaceTitle(ConsoleApi& console, const std::string& journalFolder, const InstalledGame& game, u64 titleId, u8 type, u64 ownUpdate, const ConvertOptions& options, Storage installTo, Write write, Check checkCopy, Release release) {
    ReplaceResult result;

    Journal journal;
    journal.applicationId = game.applicationId;
    journal.storage = Storage::None;
    for (auto& meta : game.metas) {
        // Whatever version of the title is there goes, along with a base game's update: the new copy takes over from both.
        bool isMain = meta.key.id == titleId && meta.key.type == type;
        bool isUpdate = ownUpdate != 0 && meta.key.id == ownUpdate && meta.key.type == Cnmt::MetaPatch;
        if (isMain || isUpdate) {
            journal.oldMetas.push_back(meta);
            for (auto& content : meta.contents)
                result.before += content.size;
        } else {
            for (auto& content : meta.contents)
                journal.kept.push_back(content.id);
        }
        if (isMain)
            journal.storage = meta.storage;
    }
    if (journal.storage == Storage::None)
        journal.storage = installTo;
    if (journal.storage != Storage::Sd && journal.storage != Storage::Nand)
        throw ToolError(format("This title is on %s, so it can't be replaced", storageName(journal.storage)));
    saveJournal(journalFolder, journal);

    InstallOutput output(console, journal, journalFolder);
    try {
        // With the checks on, every byte of it gets read back here, so none of it may stay in memory instead.
        ConvertOptions own = options;
        if (options.checkResult)
            own.holdInMemory = 0;
        write(own, output);

        if (options.checkResult) {
            checkWritten(console, journal.storage, output.written, output.hashes);
            checkCopy(journal);
        }
    } catch (...) {
        release();
        // A failure of its own isn't a power cut: the old copy is all there, so tidy up what was written and say what went wrong.
        try {
            undoWriting(console, journal);
            clearJournal(journalFolder);
        } catch (const ToolError&) {
            // It'll be finished next time the program starts.
        }
        throw;
    }
    release();

    Progress swapping("正在换入新副本", 1);
    swapping.report(0);
    journal.newKey = output.key;
    journal.installMeta = output.installMeta;
    for (auto& record : console.records(game.applicationId)) {
        bool replaced = std::any_of(journal.oldMetas.begin(), journal.oldMetas.end(), [&](const InstalledMeta& m) { return m.key == record.first; });
        if (!replaced)
            journal.newRecords.push_back(record);
    }
    journal.newRecords.insert(journal.newRecords.begin(), {journal.newKey, journal.storage});
    journal.phase = Journal::Phase::Switching;
    saveJournal(journalFolder, journal);

    finishSwitch(console, journal);
    clearJournal(journalFolder);
    swapping.done();

    result.after = output.totalBytes;
    return result;
}

}  // namespace

std::unique_ptr<FirmwareView> readGameDataOf(const SourcePtr& raw, const std::string& name, const Keyset& keyset) { return std::make_unique<OwnGameData>(raw, name, keyset); }

std::unique_ptr<FirmwareView> readLogoOf(const SourcePtr& raw, const std::string& name, const Keyset& keyset) { return std::make_unique<OwnLogo>(raw, name, keyset); }

void Replacer::checkConsole(Storage storage, const ConvertOptions& options, const Keyset& keyset) {
    constexpr const char* Label = "正在检查此主机能否读取压缩游戏";
    constexpr int Attempts = 2;
    for (int attempt = 1;; attempt++) {
        Journal journal;
        journal.storage = storage;
        saveJournal(_journalFolder, journal);
        try {
            auto files = testCopyFiles();
            SourcePtr image = buildRomFsImageOf(files);
            InstallOutput output(_console, journal, _journalFolder, Label);
            TitleConverter converter(options, keyset);
            NcaInfo info = converter.writeProbe(output, image, TestCopyId, Label);
            ContentId id;
            memcpy(id.bytes, info.hash, 16);
            try {
                auto view = _console.mountGameData(storage, id, TestCopyId);
                compareGameData(*view, image, "测试副本", Label);
            } catch (const ToolError& e) {
                throw ToolError(FirmwareRefusal + std::string(e.what()));
            }
            undoWriting(_console, journal);
            clearJournal(_journalFolder);
            return;
        } catch (const ToolError& e) {
            // The test copy goes either way, and something that's only passing is worth one more try.
            sink().message(std::string("主机检查未通过：") + e.what());
            try {
                undoWriting(_console, journal);
                clearJournal(_journalFolder);
            } catch (const ToolError&) {
                // It'll be finished next time the program starts.
            }
            if (attempt >= Attempts)
                throw;
        } catch (const Cancelled&) {
            try {
                undoWriting(_console, journal);
                clearJournal(_journalFolder);
            } catch (const ToolError&) {
            }
            throw;
        }
    }
}

/// Whatever version is on the console now is the one it has to be happy with. It's only a nicety if it can't be said: the game itself is fine.
static void letItStart(ConsoleApi& console, u64 applicationId) {
    try {
        u32 newest = 0;
        for (auto& meta : console.describe(applicationId).metas) {
            if (meta.key.type == Cnmt::MetaApplication || meta.key.type == Cnmt::MetaPatch)
                newest = std::max(newest, meta.key.version);
        }
        console.setLaunchVersion(applicationId, newest);
    } catch (const ToolError& e) {
        sink().warning(std::string("无法告知主机要启动的版本：") + e.what());
    }
}

ReplaceResult Replacer::run(const InstalledGame& game, const TitleJob& job, const ConvertOptions& options, const Keyset& keyset, Storage installTo) {
    ReplaceResult result = replaceTitle(
        _console, _journalFolder, game, job.main->meta.titleId, job.main->meta.type, job.isBaseGame() ? job.main->meta.linkedId() : 0, options, installTo,
        [&](const ConvertOptions& own, TitleOutput& output) { TitleConverter(own, keyset).run(job, output); },
        [&](const Journal& journal) { checkNewCopy(_console, job, journal, keyset); },
        [&] { job.releaseMemory(); });
    letItStart(_console, game.applicationId);
    return result;
}

ReplaceResult Replacer::run(const InstalledGame& game, const ExtractedGame& extracted, const ConvertOptions& options, const Keyset& keyset, Storage installTo) {
    ReplaceResult result = replaceTitle(
        _console, _journalFolder, game, extracted.titleId, Cnmt::MetaApplication, extracted.titleId + 0x800, options, installTo,
        [&](const ConvertOptions& own, TitleOutput& output) { TitleConverter(own, keyset).build(extracted, output); },
        [&](const Journal& journal) { checkBuiltCopy(_console, extracted, journal, keyset); },
        [] {});
    letItStart(_console, extracted.titleId);
    return result;
}

std::vector<std::string> Replacer::recover() {
    std::vector<std::string> done;
    Journal journal;
    bool found = loadJournalFile(journalPath(_journalFolder), journal);
    if (!found) {
        // The power may have gone between taking the old journal away and giving the new one its name.
        found = loadJournalFile(journalTempPath(_journalFolder), journal);
    }
    if (!found) {
        clearJournal(_journalFolder);
        return done;
    }

    if (journal.phase == Journal::Phase::Switching) {
        finishSwitch(_console, journal);
        done.push_back(format("Finished replacing a game that was interrupted (%016llx)", (unsigned long long)journal.applicationId));
    } else {
        undoWriting(_console, journal);
        if (journal.applicationId != 0)
            done.push_back(format("Cleared up after a job that was interrupted (%016llx). The game itself wasn't touched", (unsigned long long)journal.applicationId));
    }
    clearJournal(_journalFolder);
    return done;
}

}  // namespace hbp
