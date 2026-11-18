#include "title.h"

#include <deque>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <set>

#include "lz4.h"

namespace hbp {

// -------------------------------------------------------------------------------------------------------------------
// Content meta

namespace Cnmt {

std::string fileName(u64 titleId, u8 metaType) {
    const char* kind = metaType == MetaAddOnContent ? "AddOnContent" : metaType == MetaPatch ? "Patch" : "Application";
    return format("%s_%016llx.cnmt", kind, (unsigned long long)titleId);
}

}  // namespace Cnmt

ContentMeta ContentMeta::parse(Bytes bytes, const std::string& what) {
    constexpr int HeaderSize = 0x20;
    constexpr int RecordSize = 0x38;
    if (bytes.size() < HeaderSize)
        throw ToolError(what + " 的内容元数据已损坏");
    const u8* r = bytes.data();
    int extended = rd16(r + 0x0E);
    int count = rd16(r + 0x10);
    if (HeaderSize + extended + count * (i64)RecordSize > (i64)bytes.size())
        throw ToolError(what + " 的内容元数据已损坏");

    ContentMeta meta;
    for (int i = 0; i < count; i++) {
        const u8* e = r + HeaderSize + extended + i * RecordSize;
        i64 size = 0;
        for (int b = 4; b >= 0; b--)
            size = size << 8 | e[0x30 + b];
        meta.records.push_back(ContentMetaRecord{toHex(e + 0x20, 0x10), size, e[0x36], e[0x37]});
    }
    meta.titleId = rd64(r);
    meta.version = rd32(r + 8);
    meta.type = r[0x0C];
    meta.extendedHeaderSize = extended;
    meta.raw = std::move(bytes);
    return meta;
}

Bytes ContentMeta::rebuild(u32 newVersion, u32 required, const std::vector<ContentRecord>& list) const {
    size_t prefix = 0x20 + (size_t)extendedHeaderSize;
    Bytes cnmt((size_t)rebuiltSize((int)list.size()));
    u8* c = cnmt.data();
    memcpy(c, raw.data(), prefix);
    wr32(c + 8, newVersion);
    wr16(c + 0x10, (u16)list.size());
    wr16(c + 0x12, 0);
    // A base game cut down to go with its update is marked as needing it. What's made here always has all of the game, so the mark goes:
    // one that kept it had DBI warn that it needed its update, and didn't start.
    c[0x14] &= (u8)~Cnmt::AttributeCompacted;
    if ((type == Cnmt::MetaApplication || type == Cnmt::MetaPatch) && extendedHeaderSize >= 0xC)
        wr32(c + 0x28, required);
    // A game asking for a newer version of itself than the one that's in here would only have the console say to update it before it starts.
    if (type == Cnmt::MetaApplication && extendedHeaderSize >= 0x10)
        wr32(c + 0x2C, 0);
    for (size_t i = 0; i < list.size(); i++) {
        u8* r = c + prefix + i * 0x38;
        memcpy(r, list[i].nca.hash, 32);
        memcpy(r + 0x20, list[i].nca.hash, 16);
        i64 size = list[i].nca.size;
        for (int b = 0; b < 6; b++)
            r[0x30 + b] = (u8)(size >> (8 * b));
        r[0x36] = list[i].type;
        r[0x37] = list[i].idOffset;
    }
    return cnmt;
}

// -------------------------------------------------------------------------------------------------------------------
// Game files

const Archive::Entry* Archive::find(const std::string& name) const {
    for (auto& entry : _entries) {
        if (equalsNoCase(entry.name, name))
            return &entry;
    }
    return nullptr;
}

namespace {
constexpr i64 CartridgeHeaderSize = 0x200;
constexpr i64 CartridgeMagicOffset = 0x100;
constexpr i64 CartridgePartitionOffset = 0x130;
constexpr i64 KeyAreaSize = 0x1000;
constexpr int Hfs0HeaderSize = 0x10;
constexpr int Hfs0EntrySize = 0x40;
constexpr int MaxEntries = 0x10000;

std::string fileNameOf(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string stemOf(const std::string& path) {
    std::string name = fileNameOf(path);
    size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}
}  // namespace

bool FileArchive::isGameFile(const std::string& path) {
    // A Mac copying files to a card leaves "._" files beside them, which hold what Finder knows about each and aren't games.
    size_t slash = path.find_last_of("/\\");
    if (path.compare(slash == std::string::npos ? 0 : slash + 1, 2, "._") == 0)
        return false;
    return endsWithNoCase(path, ".nsp") || endsWithNoCase(path, ".nsz") || endsWithNoCase(path, ".xci") || endsWithNoCase(path, ".xcz");
}

FileArchive::FileArchive(const std::string& path) {
    _path = path;
    auto file = std::make_shared<sys::File>(path, sys::File::Mode::Read);
    _length = file->size();
    _file = std::make_shared<FileSource>(file, _length);

    u8 magic[4];
    if (_length >= 4) {
        _file->read(0, magic, 4);
        if (memcmp(magic, "PFS0", 4) == 0) {
            readPartition();
            return;
        }
    }

    // Some cartridge dumps put a key area in front of the header.
    for (i64 header : {(i64)0, KeyAreaSize}) {
        if (_length < header + CartridgeHeaderSize)
            continue;
        _file->read(header + CartridgeMagicOffset, magic, 4);
        if (memcmp(magic, "HEAD", 4) == 0) {
            readCartridge(header);
            return;
        }
    }
    throw ToolError("'" + path + "' 不是 NSP、NSZ、XCI 或 XCZ");
}

SourcePtr FileArchive::open(const Entry& entry) { return std::make_shared<SubSource>(_file, entry.offset, entry.size); }

void FileArchive::readPartition() {
    u8 head[0x10];
    if (_length < (i64)sizeof(head))
        throw ToolError("'" + _path + "' 太小，不是有效的 NSP");
    _file->read(0, head, sizeof(head));

    int count = rdi32(head + 4);
    int nameTable = rdi32(head + 8);
    i64 tableSize = count * 0x18LL + nameTable;
    if (count < 0 || count > MaxEntries || nameTable < 0 || nameTable > MostTable || !fitsWithin(0x10, tableSize, _length))
        throw ToolError("'" + _path + "' 的 NSP 头已损坏");

    Bytes table((size_t)tableSize);
    _file->read(0x10, table.data(), table.size());
    i64 dataStart = 0x10 + tableSize;
    const char* names = (const char*)table.data() + count * 0x18;
    for (int i = 0; i < count; i++) {
        const u8* e = table.data() + i * 0x18;
        i64 offset = rdi64(e);
        i64 size = rdi64(e + 8);
        int nameOffset = rdi32(e + 0x10);
        if (!fitsWithin(offset, size, _length - dataStart) || nameOffset < 0 || nameOffset >= nameTable)
            throw ToolError("'" + _path + "' 的 NSP 条目已损坏");
        _entries.push_back(Entry{std::string(names + nameOffset, strnlen(names + nameOffset, (size_t)(nameTable - nameOffset))), dataStart + offset, size});
    }
}

void FileArchive::readCartridge(i64 header) {
    u8 offset[8];
    _file->read(header + CartridgePartitionOffset, offset, 8);
    i64 root = header + rdi64(offset);
    if (root < 0 || root >= _length)
        throw ToolError("'" + _path + "' 的卡带头已损坏");

    // Only the secure partition is used, so it's the only one that has to be all there.
    for (auto& entry : readHfs0(root, false)) {
        if (entry.name == "secure") {
            _entries = readHfs0(entry.offset, true);
            return;
        }
    }
    throw ToolError("'" + _path + "' 没有 secure 分区");
}

std::vector<Archive::Entry> FileArchive::readHfs0(i64 start, bool mustFit) {
    u8 head[Hfs0HeaderSize];
    if (start < 0 || start + Hfs0HeaderSize > _length)
        throw ToolError("'" + _path + "' 被截断：有分区超出文件范围");
    _file->read(start, head, sizeof(head));
    if (memcmp(head, "HFS0", 4) != 0)
        throw ToolError("'" + _path + "' 的分区已损坏");

    int count = rdi32(head + 4);
    int nameTable = rdi32(head + 8);
    i64 tableSize = count * (i64)Hfs0EntrySize + nameTable;
    if (count < 0 || count > MaxEntries || nameTable < 0 || nameTable > MostTable || !fitsWithin(start + Hfs0HeaderSize, tableSize, _length))
        throw ToolError("'" + _path + "' 的分区已损坏");

    Bytes table((size_t)tableSize);
    _file->read(start + Hfs0HeaderSize, table.data(), table.size());
    i64 dataStart = start + Hfs0HeaderSize + tableSize;
    const char* names = (const char*)table.data() + count * Hfs0EntrySize;
    std::vector<Entry> entries;
    for (int i = 0; i < count; i++) {
        const u8* e = table.data() + i * Hfs0EntrySize;
        i64 offset = rdi64(e);
        i64 size = rdi64(e + 8);
        int nameOffset = rdi32(e + 0x10);
        if (offset < 0 || size < 0 || nameOffset < 0 || nameOffset >= nameTable)
            throw ToolError("'" + _path + "' 的分区已损坏");
        if (mustFit && !fitsWithin(offset, size, _length - dataStart))
            throw ToolError("'" + _path + "' 被截断：分区内有文件超出文件范围");
        entries.push_back(Entry{std::string(names + nameOffset, strnlen(names + nameOffset, (size_t)(nameTable - nameOffset))), dataStart + offset, size});
    }
    return entries;
}

// -------------------------------------------------------------------------------------------------------------------
// Titles

bool ContentPair::sectionAt(int index, SectionSource& out) const {
    if (patch && patch->section(index)) {
        out = SectionSource{patch.get(), patch->section(index), base.get()};
        return true;
    }
    if (base && base->section(index)) {
        out = SectionSource{base.get(), base->section(index), nullptr};
        return true;
    }
    return false;
}

std::vector<int> ContentPair::sectionsInFileOrder() const {
    std::vector<int> order;
    std::vector<std::pair<i64, int>> sorted;
    for (int i = 0; i < NcaFile::SectionCount; i++) {
        SectionSource source;
        if (sectionAt(i, source))
            sorted.emplace_back(source.section->offset, i);
    }
    std::stable_sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.first < b.first; });
    for (auto& item : sorted)
        order.push_back(item.second);
    return order;
}

void ContentPair::releaseMemory() const {
    if (base)
        base->releaseMemory();
    if (patch)
        patch->releaseMemory();
}

void TitleJob::releaseMemory() const {
    for (auto& content : contents)
        content.releaseMemory();
}

std::string TitleJob::displayName() const { return !titleName.empty() ? titleName : stemOf(main->archive->path()); }

std::string TitleJob::detail() const {
    if (!isBaseGame())
        return format("DLC v%u", main->meta.version);
    return update ? format("base v%u + update v%u, repacked as one title", main->meta.version, update->meta.version) : format("base v%u", main->meta.version);
}

std::string TitleJob::describe() const { return format("%s [%016llX]: %s", displayName().c_str(), (unsigned long long)main->meta.titleId, detail().c_str()); }

std::string TitleJob::stem(u32 version, bool sharedFile) const {
    unsigned long long id = main->meta.titleId;
    if (isBaseGame() && !titleName.empty())
        return format("%s [%016llX][v%u]", LocalNames::forOutputName(titleName).c_str(), id, version);
    std::string stem = LocalNames::forOutputName(stemOf(main->archive->path()));
    return sharedFile ? format("%s [%016llX]", stem.c_str(), id) : stem;
}

TitleSet::TitleSet(const Keyset& keyset) : _keyset(keyset), _tickets(keyset) {}

bool TitleSet::sharesFile(const TitleJob& job) const {
    int count = 0;
    for (auto& item : _items) {
        if (item->archive == job.main->archive && item->meta.type != Cnmt::MetaPatch)
            count++;
    }
    return count > 1;
}

std::vector<std::string> TitleSet::expand(const std::vector<std::string>& inputs, std::vector<std::string>& notes, std::vector<InputIssue>& problems, const std::vector<std::string>& foldersToSkip, bool skipExtracted, bool ownOutputToo) {
    constexpr const char* OwnOutputMarker = "[lz4]";
    std::vector<std::string> files;
    std::set<std::string> seen;
    auto add = [&](const std::string& path) {
        if (seen.insert(path).second)
            files.push_back(path);
    };

    for (auto& input : inputs) {
        if (sys::isDirectory(input)) {
            std::vector<std::string> found;
            std::vector<std::string> pending{input};
            while (!pending.empty()) {
                std::string folder = pending.back();
                pending.pop_back();
                checkStop();
                auto entries = sys::list(folder);
                if (skipExtracted) {
                    auto has = [&](const char* name) { return std::any_of(entries.begin(), entries.end(), [&](const sys::DirEntry& e) { return e.isDirectory && equalsNoCase(e.name, name); }); };
                    if (has("exefs") && has("control"))
                        continue;
                }
                for (auto& entry : entries) {
                    std::string path = folder + (folder.back() == '/' ? "" : "/") + entry.name;
                    if (entry.isDirectory) {
                        if (std::none_of(foldersToSkip.begin(), foldersToSkip.end(), [&](const std::string& skip) { return equalsNoCase(path, skip); }))
                            pending.push_back(path);
                    } else if (FileArchive::isGameFile(path))
                        found.push_back(path);
                }
            }
            std::sort(found.begin(), found.end(), [](const std::string& a, const std::string& b) {
                return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) { return tolower((u8)x) < tolower((u8)y); });
            });
            // Output from an earlier run sits beside its source, and would otherwise count as a second copy of the title.
            int skipped = 0;
            for (auto& path : found) {
                if (!ownOutputToo && containsNoCase(fileNameOf(path), OwnOutputMarker))
                    skipped++;
                else
                    add(path);
            }
            if (skipped > 0)
                notes.push_back(format("Skipped %d file(s) in '%s' that this tool already compressed", skipped, input.c_str()));
        } else if (!sys::exists(input)) {
            problems.push_back(InputIssue{input, "'" + input + "' 未找到"});
        } else {
            add(input);
        }
    }
    return files;
}

/// Sphaira's forwarders have the wrong size recorded for their content meta, so that a dump of one only holds the start of it. All that was in it
/// can be told from a program and its control data, as it's the same for all of them.
ItemPtr TitleSet::itemFromContents(const ArchivePtr& archive, const Archive::Entry& metaEntry) {
    std::string why = format("%s is only %lld bytes, where an NCA takes at least 3 KB", metaEntry.name.c_str(), (long long)metaEntry.size);
    constexpr int HeaderSize = 0x20, ExtendedSize = 0x10, RecordSize = 0x38, DigestSize = 0x20, Parts = 2;
    NcaPtr program;
    const Archive::Entry* entries[Parts] = {nullptr, nullptr};
    u64 id = 0;
    for (auto& entry : archive->entries()) {
        if (&entry == &metaEntry || endsWithNoCase(entry.name, ".tik") || endsWithNoCase(entry.name, ".cert"))
            continue;
        if (!endsWithNoCase(entry.name, ".nca"))
            throw ToolError(why);
        auto nca = std::make_shared<NcaFile>(archive->open(entry), entry.name, _keyset, _tickets);
        int part = nca->contentType() == NcaContentType::Program ? 0 : nca->contentType() == NcaContentType::Control ? 1 : -1;
        if (part < 0 || entries[part] || (id != 0 && id != nca->programId()))
            throw ToolError(why);
        entries[part] = &entry;
        id = nca->programId();
        if (part == 0)
            program = nca;
    }
    if (!entries[0] || !entries[1])
        throw ToolError(why);

    Bytes raw((size_t)(HeaderSize + ExtendedSize + Parts * RecordSize + DigestSize));
    u8* r = raw.data();
    wr64(r, id);
    r[0x0C] = Cnmt::MetaApplication;
    wr16(r + 0x0E, ExtendedSize);
    wr16(r + 0x10, Parts);
    wr64(r + HeaderSize, id | 0x800);
    for (int part = 0; part < Parts; part++) {
        u8* e = r + HeaderSize + ExtendedSize + part * RecordSize;
        const Archive::Entry& entry = *entries[part];
        Bytes contentId;
        if (!fromHex(entry.name.substr(0, entry.name.size() - 4), contentId) || contentId.size() != 0x10)
            throw ToolError(why);
        memcpy(e + 0x20, contentId.data(), contentId.size());
        for (int b = 0; b < 6; b++)
            e[0x30 + b] = (u8)(entry.size >> (8 * b));
        e[0x36] = part == 0 ? Cnmt::ContentTypeProgram : Cnmt::ContentTypeControl;
    }

    auto item = std::make_shared<TitleItem>();
    item->archive = archive;
    item->metaName = metaEntry.name;
    item->metaNca = program;
    item->meta = ContentMeta::parse(std::move(raw), metaEntry.name);
    return item;
}

void TitleSet::add(const ArchivePtr& archive) {
    std::string shown = fileNameOf(archive->path());
    try {
        _archives.push_back(archive);
        constexpr i64 MaxTicketSize = 0x4000;
        for (auto& entry : archive->entries()) {
            if (!endsWithNoCase(entry.name, ".tik") || entry.size > MaxTicketSize)
                continue;
            Bytes ticket((size_t)entry.size);
            archive->open(entry)->read(0, ticket.data(), ticket.size());
            _tickets.addTicket(ticket.data(), ticket.size());
        }

        int metas = 0;
        for (auto& entry : archive->entries()) {
            if (!endsWithNoCase(entry.name, ".cnmt.nca"))
                continue;
            metas++;
            if (entry.size < 0xC00) {
                _items.push_back(itemFromContents(archive, entry));
                continue;
            }
            auto nca = std::make_shared<NcaFile>(archive->open(entry), entry.name, _keyset, _tickets);
            const NcaFsSection* section = nullptr;
            for (int i = 0; i < NcaFile::SectionCount && !section; i++) {
                if (nca->section(i) && nca->section(i)->fsType() == NcaFsSection::FsPartition)
                    section = nca->section(i);
            }
            if (!section)
                throw ToolError(entry.name + " 没有 PFS0 区段");
            auto pfs = nca->openPfs0(*section, nullptr);
            const Pfs0Archive::Entry* cnmt = nullptr;
            for (auto& file : pfs->files()) {
                if (endsWithNoCase(file.name, ".cnmt"))
                    cnmt = &file;
            }
            if (!cnmt)
                throw ToolError(entry.name + " 中没有 .cnmt 文件");
            auto item = std::make_shared<TitleItem>();
            item->archive = archive;
            item->metaName = entry.name;
            item->metaNca = nca;
            item->meta = ContentMeta::parse(pfs->readAll(*cnmt), entry.name);
            _items.push_back(item);
        }
        if (metas == 0)
            problems.push_back(InputIssue{archive->path(), "'" + shown + "' 没有内容元数据，因此不是可安装的游戏"});
    } catch (const ToolError& e) {
        problems.push_back(InputIssue{archive->path(), "'" + shown + "': " + e.what()});
    } catch (const std::exception& e) {
        problems.push_back(InputIssue{archive->path(), "'" + shown + "': " + e.what()});
    }
}

void TitleSet::group(bool everyFile) {
    std::vector<ItemPtr> updates;
    for (auto& item : _items) {
        if (item->meta.type == Cnmt::MetaPatch)
            updates.push_back(item);
    }
    std::set<const TitleItem*> used;

    auto groupOf = [&](u8 type) {
        std::vector<std::pair<u64, std::vector<ItemPtr>>> groups;
        for (auto& item : _items) {
            if (item->meta.type != type)
                continue;
            auto found = std::find_if(groups.begin(), groups.end(), [&](auto& g) { return g.first == item->meta.titleId; });
            if (found == groups.end()) {
                groups.push_back({item->meta.titleId, {item}});
            } else {
                found->second.push_back(item);
            }
        }
        return groups;
    };

    for (auto& group : groupOf(Cnmt::MetaApplication)) {
        std::vector<ItemPtr> candidates;
        for (auto& update : updates) {
            if (update->meta.linkedId() == group.first) {
                candidates.push_back(update);
                used.insert(update.get());
            }
        }
        ItemPtr update;
        if (!candidates.empty())
            update = newestAndNoteOthers(candidates, "update");

        if (everyFile) {
            std::vector<ItemPtr> copies = group.second;
            std::stable_sort(copies.begin(), copies.end(), [](const ItemPtr& a, const ItemPtr& b) { return a->meta.version > b->meta.version; });
            auto partner = std::find_if(copies.begin(), copies.end(), [&](const ItemPtr& copy) { return update && updateGoesWith(*update, *copy); });
            if (update && partner == copies.end())
                notes.push_back(format("Ignored update v%u for %016llX: the base file is already that new", update->meta.version, (unsigned long long)group.first));
            for (auto it = copies.begin(); it != copies.end(); ++it)
                addJob(*it, it == partner ? update : nullptr);
            continue;
        }

        ItemPtr main = newestAndNoteOthers(group.second, "本体游戏");
        if (update && !updateGoesWith(*update, *main)) {
            notes.push_back(format("Ignored update v%u for %016llX: the base file is already v%u", update->meta.version, (unsigned long long)main->meta.titleId, main->meta.version));
            update = nullptr;
        }
        addJob(main, update);
    }

    for (auto& group : groupOf(Cnmt::MetaAddOnContent)) {
        if (everyFile) {
            for (auto& item : group.second)
                addJob(item, nullptr);
        } else {
            addJob(newestAndNoteOthers(group.second, "DLC"), nullptr);
        }
    }

    for (auto& update : updates) {
        if (!used.count(update.get()))
            problems.push_back(InputIssue{update->archive->path(), format("'%s' is an update (v%u) without its base game %016llX. Updates can't be rebuilt alone: add the base game to the same run", fileNameOf(update->archive->path()).c_str(), update->meta.version, (unsigned long long)update->meta.linkedId())});
    }

    for (auto& other : _items) {
        u8 type = other->meta.type;
        if (type != Cnmt::MetaApplication && type != Cnmt::MetaPatch && type != Cnmt::MetaAddOnContent)
            problems.push_back(InputIssue{other->archive->path(), format("'%s' holds content meta type 0x%02x, which isn't supported", fileNameOf(other->archive->path()).c_str(), type)});
    }
}

namespace {

/// The name and icon of a Control NCA, reading just them. They're only there to look at, and looking at them doesn't take checking them.
void readNameAndIcon(NcaFile& nca, TitleSummary& out) {
    constexpr i64 MaxIconSize = 1 << 20;
    const NcaFsSection* section = nullptr;
    for (int i = 0; i < NcaFile::SectionCount && !section; i++) {
        if (nca.section(i) && nca.section(i)->fsType() == NcaFsSection::FsRomFs)
            section = nca.section(i);
    }
    if (!section)
        return;
    SourcePtr image = nca.openRomFsImage(*section, nullptr, false);
    RomFsArchive archive = RomFsArchive::open(image, nca.name());
    const RomFsArchive::Entry* best = nullptr;
    for (auto& file : archive.files) {
        if (file.path == "control.nacp") {
            Bytes nacp((size_t)std::min<i64>(file.size, Nacp::Size));
            image->read(file.offset, nacp.data(), nacp.size());
            out.name = Nacp::choose(Nacp::titles(nacp.data(), nacp.size())).name;
            out.displayVersion = Nacp::displayVersion(nacp.data(), nacp.size());
        } else if (file.size > 0 && file.size <= MaxIconSize && file.path.rfind("icon_", 0) == 0 && endsWithNoCase(file.path, ".dat") && (!best || file.path == "icon_AmericanEnglish.dat")) {
            best = &file;
        }
    }
    if (best) {
        out.icon.resize((size_t)best->size);
        image->read(best->offset, out.icon.data(), out.icon.size());
    }
    // What a copy this tool made says about what the game came to before.
    for (auto& file : archive.files) {
        if (file.path != CopyMark::FileName || file.size > 4096)
            continue;
        Bytes data((size_t)file.size);
        image->read(file.offset, data.data(), data.size());
        CopyMark mark;
        if (CopyMark::parse(data, mark))
            out.originalBytes = mark.originalBytes;
    }
}

/// Whether this tool compressed a content: a compressed section of its game data with key generation 0, which its headers say.
bool compressedHere(NcaFile& nca) {
    for (int i = 0; i < NcaFile::SectionCount; i++) {
        const NcaFsSection* section = nca.section(i);
        if (section && section->hasCompression() && nca.isRebuiltCompressed(*section))
            return true;
    }
    return false;
}

}  // namespace

std::vector<TitleSummary> TitleSet::summarize() {
    std::vector<TitleSummary> list;
    for (auto& item : _items) {
        TitleSummary summary;
        summary.titleId = item->meta.titleId;
        summary.type = item->meta.type;
        summary.version = item->meta.version;
        summary.linkedId = item->meta.linkedId();
        summary.bytes = inputBytesOf(item, nullptr);
        for (auto& record : item->meta.records) {
            try {
                // What the game data is says whether this tool compressed it, rather than the file's name.
                if (record.type == Cnmt::ContentTypeProgram || record.type == Cnmt::ContentTypeData) {
                    NcaPtr nca = openContent(*item, record);
                    if (record.type == Cnmt::ContentTypeProgram && item->meta.type == Cnmt::MetaApplication)
                        summary.holdsOnlyPart = summary.holdsOnlyPart || nca->holdsOnlyPart();
                    summary.madeHere = summary.madeHere || compressedHere(*nca);
                    summary.solidBytes = std::max(summary.solidBytes, nca->solidBytes());
                } else if (record.type == Cnmt::ContentTypeControl) {
                    readNameAndIcon(*openContent(*item, record), summary);
                }
            } catch (const ToolError&) {
                // A name and an icon are only niceties. Any real damage gets reported when the title itself is gone through.
            }
        }
        list.push_back(std::move(summary));
    }
    return list;
}

bool TitleSet::holdsOnlyPart(const TitleItem& item) {
    for (auto& record : item.meta.records) {
        if (record.type == Cnmt::ContentTypeProgram && openContent(item, record)->holdsOnlyPart())
            return true;
    }
    return false;
}

/// An update is newer than the base game it's for, unless that base game was cut down to go with it, in which case they have the same version.
bool TitleSet::updateGoesWith(const TitleItem& update, const TitleItem& base) {
    if (update.meta.version != base.meta.version)
        return update.meta.version > base.meta.version;
    try {
        return holdsOnlyPart(base);
    } catch (const ToolError&) {
        return false;
    }
}

ItemPtr TitleSet::newestAndNoteOthers(std::vector<ItemPtr> items, const char* kind) {
    std::stable_sort(items.begin(), items.end(), [](const ItemPtr& a, const ItemPtr& b) { return a->meta.version > b->meta.version; });
    for (size_t i = 1; i < items.size(); i++)
        notes.push_back(format("Using %s v%u of %016llX, ignoring v%u in '%s'", kind, items[0]->meta.version, (unsigned long long)items[0]->meta.titleId, items[i]->meta.version, fileNameOf(items[i]->archive->path()).c_str()));
    return items[0];
}

static bool isCopied(const ContentMetaRecord& record) { return record.type != Cnmt::ContentTypeMeta && record.type != Cnmt::ContentTypeDeltaFragment; }

void TitleSet::addJob(const ItemPtr& main, const ItemPtr& update) {
    checkStop();
    try {
        std::vector<ContentPair> contents;
        for (auto& record : main->meta.records) {
            if (isCopied(record))
                contents.push_back(ContentPair{record.type, record.idOffset, openContent(*main, record), nullptr});
        }

        if (update) {
            for (auto& record : update->meta.records) {
                if (!isCopied(record))
                    continue;
                NcaPtr patch = openContent(*update, record);
                auto found = std::find_if(contents.begin(), contents.end(), [&](const ContentPair& c) { return c.type == record.type && c.idOffset == record.idOffset; });
                if (found != contents.end())
                    found->patch = patch;
                else
                    contents.push_back(ContentPair{record.type, record.idOffset, nullptr, patch});
            }
        }

        // Whatever an update doesn't take over has to be all there.
        for (auto& content : contents) {
            for (int i = 0; i < NcaFile::SectionCount; i++) {
                SectionSource source;
                if (content.sectionAt(i, source) && source.section->hasSparse())
                    throw ToolError("这只包含游戏的一部分，其余内容在更新补丁里，两者缺一不可");
            }
        }

        // A base cut down to go with its update is the original, as far as it goes, and has the same version as the update.
        if (update && !holdsOnlyPart(*main))
            keepOriginal(contents, main, update);

        TitleJob job;
        job.main = main;
        job.update = update;
        job.inputBytes = inputBytesOf(main, update);
        job.titleName = readTitleName(contents);
        job.contents = std::move(contents);
        jobs.push_back(std::move(job));
    } catch (const ToolError& e) {
        problems.push_back(InputIssue{main->archive->path(), "'" + fileNameOf(main->archive->path()) + "': " + e.what()});
    } catch (const std::exception& e) {
        problems.push_back(InputIssue{main->archive->path(), "'" + fileNameOf(main->archive->path()) + "': " + e.what()});
    }
}

void TitleSet::keepOriginal(std::vector<ContentPair>& contents, const ItemPtr& main, const ItemPtr& update) {
    UpdateMap map;
    bool have = false;
    try {
        for (auto& item : _items) {
            if (have || item == update || item->meta.type != Cnmt::MetaPatch || item->meta.linkedId() != main->meta.titleId || item->meta.version != main->meta.version)
                continue;
            map.version = item->meta.version;
            for (auto& record : item->meta.records) {
                if (!isCopied(record))
                    continue;
                NcaPtr nca = openContent(*item, record);
                for (int i = 0; i < NcaFile::SectionCount; i++) {
                    const NcaFsSection* section = nca->section(i);
                    if (section && section->hasIndirect() && section->fsType() == NcaFsSection::FsRomFs)
                        map.sections.push_back(KeptSection{record.type, record.idOffset, i, nca->keptInImage(*section, nullptr)});
                }
            }
            have = !map.sections.empty();
        }
        if (!have)
            have = UpdateMap::inControl(contents, main->meta.version, map);
    } catch (const ToolError&) {
        // Without it, a copy that has an update in it already doesn't fit a newer one, which its hash tree says.
        have = false;
    }
    if (!have)
        return;
    for (auto& section : map.sections) {
        for (auto& content : contents) {
            if (content.type == section.contentType && content.idOffset == section.idOffset && content.base)
                content.base->keptOriginal.push_back(section);
        }
    }
}

bool UpdateMap::inControl(const std::vector<ContentPair>& contents, u32 version, UpdateMap& out) {
    for (auto& content : contents) {
        if (content.type != Cnmt::ContentTypeControl || !content.base)
            continue;
        for (int i = 0; i < NcaFile::SectionCount; i++) {
            const NcaFsSection* section = content.base->section(i);
            if (!section || section->fsType() != NcaFsSection::FsRomFs)
                continue;
            SourcePtr image = content.base->openRomFsImage(*section, nullptr);
            RomFsArchive archive = RomFsArchive::open(image, content.base->name());
            for (auto& file : archive.files) {
                if (file.path != UpdateMap::FileName || file.size > (64 << 20))
                    continue;
                Bytes data((size_t)file.size);
                image->read(file.offset, data.data(), data.size());
                if (parse(data, out) && out.version == version)
                    return true;
            }
        }
    }
    out = UpdateMap();
    return false;
}

bool UpdateMap::forJob(const TitleJob& job, bool compress, UpdateMap& out) {
    out = UpdateMap();
    // A copy made again, compressed, with no update of its own keeps the map it was made with, as its next update needs it. Uncompressed, its
    // control data goes in as it is, map and all.
    if (!job.update)
        return compress && inControl(job.contents, job.main->meta.version, out);
    out.version = job.update->meta.version;
    for (auto& content : job.contents) {
        if (!content.base || !content.patch)
            continue;
        for (int i = 0; i < NcaFile::SectionCount; i++) {
            const NcaFsSection* section = content.patch->section(i);
            if (section && section->hasIndirect() && section->fsType() == NcaFsSection::FsRomFs)
                out.sections.push_back(KeptSection{content.type, content.idOffset, i, content.patch->keptInImage(*section, content.base.get())});
        }
    }
    return !out.sections.empty();
}

Bytes UpdateMap::toBytes() const {
    std::string text = "hacBrewPackLZ update map 1\n";
    text += format("update %u\n", version);
    for (auto& section : sections) {
        text += format("section %d %d %d %d\n", section.contentType, section.idOffset, section.index, (int)section.ranges.size());
        for (auto& range : section.ranges)
            text += format("%llx %llx %llx\n", (unsigned long long)range.original, (unsigned long long)range.image, (unsigned long long)range.length);
    }
    return Bytes(text.begin(), text.end());
}

bool UpdateMap::parse(const Bytes& data, UpdateMap& out) {
    out = UpdateMap();
    std::vector<std::string> lines;
    std::string text(data.begin(), data.end());
    for (size_t at = 0; at <= text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        lines.push_back(text.substr(at, end - at));
        at = end + 1;
    }
    if (lines.size() < 2 || lines[0] != "hacBrewPackLZ update map 1" || sscanf(lines[1].c_str(), "update %u", &out.version) != 1)
        return false;
    size_t at = 2;
    while (at < lines.size() && lines[at].rfind("section ", 0) == 0) {
        int type, idOffset, index, count;
        if (sscanf(lines[at++].c_str(), "section %d %d %d %d", &type, &idOffset, &index, &count) != 4 || count < 0 || at + (size_t)count > lines.size())
            return false;
        KeptSection section{(u8)type, (u8)idOffset, index, {}};
        for (int i = 0; i < count; i++) {
            unsigned long long o, m, n;
            if (sscanf(lines[at++].c_str(), "%llx %llx %llx", &o, &m, &n) != 3)
                return false;
            section.ranges.push_back(KeptRange{(i64)o, (i64)m, (i64)n});
        }
        out.sections.push_back(std::move(section));
    }
    return true;
}

/// The size of the files a title came from, or if a file holds other titles too, just this title's share of it.
i64 TitleSet::inputBytesOf(const ItemPtr& main, const ItemPtr& update) const {
    i64 total = 0;
    std::vector<ItemPtr> items{main};
    if (update)
        items.push_back(update);
    std::vector<Archive*> archives;
    for (auto& item : items) {
        if (std::find(archives.begin(), archives.end(), item->archive.get()) == archives.end())
            archives.push_back(item->archive.get());
    }

    for (Archive* archive : archives) {
        bool others = std::any_of(_items.begin(), _items.end(), [&](const ItemPtr& i) { return i->archive.get() == archive && i != main && i != update; });
        if (!others) {
            total += archive->length();
            continue;
        }
        std::set<std::string> mine;
        for (auto& item : items) {
            if (item->archive.get() != archive)
                continue;
            for (auto& record : item->meta.records)
                mine.insert(record.id);
            if (const Archive::Entry* entry = archive->find(item->metaName))
                total += entry->size;
        }
        for (auto& entry : archive->entries()) {
            if (mine.count(stemOf(entry.name)))
                total += entry.size;
        }
    }
    return total;
}

NcaPtr TitleSet::openContent(const TitleItem& item, const ContentMetaRecord& record) {
    std::string name = record.id + ".nca";
    if (const Archive::Entry* entry = item.archive->find(name))
        return std::make_shared<NcaFile>(item.archive->open(*entry), name, _keyset, _tickets);

    std::string compressed = record.id + ".ncz";
    const Archive::Entry* packed = item.archive->find(compressed);
    if (!packed)
        throw ToolError("'" + fileNameOf(item.archive->path()) + "' 缺少 " + name + "，而该内容在其内容元数据中有列出");
    auto source = std::make_shared<NczSource>(item.archive->open(*packed), compressed);
    if (source->length() != record.size)
        throw ToolError(format("%s would decompress to %lld bytes, but its content meta says %lld. The file is damaged", compressed.c_str(), (long long)source->length(), (long long)record.size));
    return std::make_shared<NcaFile>(source, name, _keyset, _tickets);
}

std::string TitleSet::readTitleName(const std::vector<ContentPair>& contents) {
    try {
        for (auto& control : contents) {
            if (control.type != Cnmt::ContentTypeControl)
                continue;
            NcaFile& nca = control.primary();
            const NcaFsSection* section = nullptr;
            for (int i = 0; i < NcaFile::SectionCount && !section; i++) {
                if (nca.section(i) && nca.section(i)->fsType() == NcaFsSection::FsRomFs)
                    section = nca.section(i);
            }
            if (!section)
                return "";

            // It only says what a title's called, which isn't worth reading all of the rest to check.
            SourcePtr image = nca.openRomFsImage(*section, control.patch ? control.base.get() : nullptr, false);
            RomFsArchive archive = RomFsArchive::open(image, nca.name());
            for (auto& file : archive.files) {
                if (file.path != "control.nacp")
                    continue;
                Bytes nacp((size_t)std::min<i64>(file.size, Nacp::Size));
                image->read(file.offset, nacp.data(), nacp.size());
                return Nacp::choose(Nacp::titles(nacp.data(), nacp.size())).name;
            }
            return "";
        }
    } catch (const ToolError&) {
        // A name is only a nicety. Any real damage gets reported when the title itself is processed.
    }
    return "";
}

// -------------------------------------------------------------------------------------------------------------------
// NSP output

namespace {
constexpr int PfsEntrySize = 0x18;
constexpr int NcaNameLength = 36;      // 32 hex digits + ".nca"
constexpr int MetaNcaNameLength = 41;  // 32 hex digits + ".cnmt.nca"
}  // namespace

i64 NspOutput::headerSize(int contentCount, int titleCount) {
    int nameTable = (contentCount * (NcaNameLength + 1) + titleCount * (MetaNcaNameLength + 1) + 0x1F) & ~0x1F;
    return 0x10 + (contentCount + titleCount) * PfsEntrySize + nameTable;
}

NspOutput::NspOutput(std::string path, int contentCount, i64 sizeHint, bool replace, int titleCount)
    : _path(std::move(path)), _partPath(_path + ".part"), _file(_partPath, sys::File::Mode::Create, sizeHint), _dataStart(headerSize(contentCount, titleCount)),
      _expectedFiles(contentCount + titleCount), _titlesLeft(titleCount), _position(_dataStart), _replace(replace) {
    Bytes reserved((size_t)_dataStart);
    _file.write(0, reserved.data(), reserved.size());
    _setsAside = sizeHint > 0;
}

namespace {
/// Steps that keep the file in few pieces, while each change to the card's allocation table stays small.
constexpr i64 SetAsideStep = 1LL << 30;
constexpr i64 FlushEvery = 256LL << 20;
}  // namespace

void NspOutput::makeRoomFor(i64 end) {
    if (!_setsAside || end <= _setAside)
        return;
    i64 wanted = std::max(end, _setAside + SetAsideStep);
    // Near the end of what's free, it can still fit if it doesn't ask for more than it needs.
    if (_file.reserve(wanted) || (wanted > end && _file.reserve(wanted = end)))
        _setAside = wanted;
    else
        _setsAside = false;
}

NspOutput::~NspOutput() {
    if (!_finished) {
        _file.close();
        sys::removeFile(_partPath);
    }
}

NcaStream& NspOutput::beginNca(const char*, i64, const char*) {
    _ncaStart = _position;
    return *this;
}

void NspOutput::write(const void* data, size_t size) {
    makeRoomFor(_position + (i64)size);
    _file.write(_position, data, size);
    _position += (i64)size;
    _unflushed += (i64)size;
    if (_unflushed >= FlushEvery) {
        _file.flush();
        _unflushed = 0;
    }
}

void NspOutput::overwrite(i64 offset, const void* data, size_t size) { _file.write(_ncaStart + offset, data, size); }

void NspOutput::readBack(i64 offset, void* data, size_t size) { _file.read(_ncaStart + offset, data, size); }

void NspOutput::endNca(const NcaInfo& info, const char* extension) { _files.push_back(FileRecord{info.id() + extension, _ncaStart - _dataStart, info.size}); }

void NspOutput::finish(const ContentMeta&, const NcaInfo&) {
    // A game's DLC goes on in the same file after it.
    if (--_titlesLeft > 0)
        return;
    if ((int)_files.size() != _expectedFiles)
        throw ToolError(format("Internal error: an NSP reserved %d files but received %d", _expectedFiles, (int)_files.size()));

    std::string strings;
    std::vector<int> nameOffsets;
    for (auto& file : _files) {
        nameOffsets.push_back((int)strings.size());
        strings += file.name;
        strings += '\0';
    }
    int nameTable = (int)(_dataStart - 0x10 - (i64)_files.size() * PfsEntrySize);
    if ((int)strings.size() > nameTable)
        throw ToolError("内部错误：PFS0 名称表预留空间不足");

    Bytes header((size_t)_dataStart);
    u8* h = header.data();
    memcpy(h, "PFS0", 4);
    wr32(h + 4, (u32)_files.size());
    wr32(h + 8, (u32)nameTable);
    for (size_t i = 0; i < _files.size(); i++) {
        u8* e = h + 0x10 + i * PfsEntrySize;
        wr64(e, (u64)_files[i].offset);
        wr64(e + 8, (u64)_files[i].size);
        wr32(e + 0x10, (u32)nameOffsets[i]);
    }
    memcpy(h + 0x10 + _files.size() * PfsEntrySize, strings.data(), strings.size());
    _file.write(0, header.data(), header.size());
    if (_setAside > 0)
        _file.setSize(_position);
    _file.flush();
    _file.close();
    // Nothing that's there already is deleted to make way for it, unless that was asked for: if the name were then turned down, both would
    // be gone, as an unfinished file is deleted.
    if (_replace)
        sys::removeFile(_path);
    // The whole job's work is in it by now, so a request the card turns down is asked again a few times, a moment apart, before it's given up.
    constexpr int RenameTries = 4;
    for (int tries = 1;; tries++) {
        try {
            sys::renameFile(_partPath, _path);
            break;
        } catch (const ToolError& e) {
            if (tries >= RenameTries || !refusedBySystem(e.what()))
                throw;
            sys::sleepMs(1000 * tries);
        }
    }
    sys::commit(_path);
    _finished = true;
}

Bytes readTitleIcon(const TitleJob& job, bool verify) {
    constexpr i64 MaxIconSize = 1 << 20;
    try {
        for (auto& control : job.contents) {
            if (control.type != Cnmt::ContentTypeControl)
                continue;
            NcaFile& nca = control.primary();
            const NcaFsSection* section = nullptr;
            for (int i = 0; i < NcaFile::SectionCount && !section; i++) {
                if (nca.section(i) && nca.section(i)->fsType() == NcaFsSection::FsRomFs)
                    section = nca.section(i);
            }
            if (!section)
                return {};

            SourcePtr image = nca.openRomFsImage(*section, control.patch ? control.base.get() : nullptr, verify);
            RomFsArchive archive = RomFsArchive::open(image, nca.name());
            const RomFsArchive::Entry* best = nullptr;
            for (auto& file : archive.files) {
                if (file.size <= 0 || file.size > MaxIconSize || file.path.rfind("icon_", 0) != 0 || !endsWithNoCase(file.path, ".dat"))
                    continue;
                if (!best || file.path == "icon_AmericanEnglish.dat")
                    best = &file;
            }
            if (!best)
                return {};
            Bytes icon((size_t)best->size);
            image->read(best->offset, icon.data(), icon.size());
            return icon;
        }
    } catch (const ToolError&) {
        // A game file with a damaged icon is still a game file.
    }
    return {};
}

std::string outputFileName(const TitleJob& job, const TitleSet& set, const ConvertOptions& options) {
    u32 version = job.update ? job.update->meta.version : job.main->meta.version;
    std::string stem = job.stem(version, set.sharesFile(job));
    std::string suffix = !options.compress ? "" : (!stem.empty() && stem.back() == ']') ? "[LZ4]" : " [LZ4]";
    return stem + suffix + ".nsp";
}

// -------------------------------------------------------------------------------------------------------------------
// Converting

/// Less game data than this isn't worth compressing: the tables saying where each piece went take more room than it could save, as with
/// a DLC that's only a few bytes, which came out at 48 KB.
static constexpr i64 MinGameDataToCompress = 1 << 20;

TitleConverter::TitleConverter(ConvertOptions options, const Keyset& keyset) : _options(std::move(options)) {
    if (_options.threads <= 0)
        _options.threads = sys::workerCount();
    const u8* keyAreaKey = keyset.keyAreaKey(0, 0);
    if (!keyAreaKey)
        throw ToolError("密钥文件中缺少 key_area_key_application_00");
    static const u8 SectionKey[16] = {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};
    _writer.reset(new NcaWriter(keyset.headerKey(), keyAreaKey, SectionKey, 1));
}

static bool isExefs(const ContentPair& content, const NcaFsSection& section) { return content.type == Cnmt::ContentTypeProgram && section.index == 0; }

TitleConverter::Plan TitleConverter::plan(const ContentPair& content, const SectionSource& source, const ConvertOptions& options) {
    const NcaFsSection& section = *source.section;
    NcaContentType type = source.owner->contentType();
    bool holdsGameData = type == NcaContentType::Program || type == NcaContentType::Data || type == NcaContentType::PublicData;
    bool isPatched = section.hasIndirect() || section.encryption() == NcaFsSection::EncryptCtrEx;

    if (section.fsType() == NcaFsSection::FsRomFs) {
        if (options.keepCompressed && holdsGameData && section.hasCompression() && !isPatched)
            return Plan::PassThrough;
        // Too little to gain anything, it stays as it is, compressed by Nintendo or not, unless an update has to be made part of it.
        i64 data = section.dataLevelSize();
        if (holdsGameData && options.compress && data > 0 && data < MinGameDataToCompress)
            return isPatched ? Plan::RebuildRomFs : Plan::PassThrough;
        if (holdsGameData && options.compress)
            return Plan::CompressRomFs;
        return isPatched || (holdsGameData && section.hasCompression()) ? Plan::RebuildRomFs : Plan::PassThrough;
    }
    return (isExefs(content, section) && !options.noSign) || isPatched ? Plan::RebuildPfs0 : Plan::PassThrough;
}

void TitleConverter::run(const TitleJob& job, TitleOutput& output) {
    NczSource::setSpoolFolder(_options.tempDir);
    u32 version = job.update ? job.update->meta.version : job.main->meta.version;

    struct Release {
        const TitleJob& job;
        ~Release() { job.releaseMemory(); }
    } release{job};

    bool compressed = false;
    std::vector<ContentRecord> records;
    // The map goes in compressed or not: a newer update is made part of either with it, here over what's installed.
    _hasMap = UpdateMap::forJob(job, _options.compress, _map);
    _hasMark = _options.compress;
    if (_hasMark)
        _mark = CopyMark::forJob(job, _options);
    for (auto& content : job.contents)
        records.push_back(ContentRecord{writeContent(output, content, compressed), content.type, content.idOffset});

    const ContentMeta& meta = job.main->meta;
    u32 required = std::min(std::max(meta.requiredSystemVersion(), job.update ? job.update->meta.requiredSystemVersion() : 0u), _options.consoleRequiredSystem);
    if (compressed)
        required = std::max(required, CompressionMinSystemVersion);
    Bytes cnmt = meta.rebuild(version, required, records);

    SourcePtr pfs0 = buildPfs0({{Cnmt::fileName(meta.titleId, meta.type), bytesSource(cnmt)}});
    NcaSections sections(1);
    sections[0] = Sections::pfs0(pfs0, Sections::MetaHashBlockSize, true);
    NcaStream& stream = output.beginNca("Meta", NcaWriter::totalSize(sections), ".cnmt.nca");
    NcaInfo metaInfo = _writer->write(stream, NcaContentType::Meta, meta.titleId, sections, false, nullptr, 0, job.main->metaNca->sdkVersion());
    output.endNca(metaInfo, ".cnmt.nca");

    output.finish(ContentMeta::parse(cnmt, "重建后的内容元数据"), metaInfo);
}

/// What hacBrewPack puts in a build unless told otherwise.
static constexpr u32 BuildSdkVersion = 0x000C1100;

/// All that compressing takes on top of what depends on how large the game is.
static constexpr i64 MemoryForTheRest = 192LL << 20;

static const char* labelOf(const ContentPair& content) {
    switch (content.type) {
        case Cnmt::ContentTypeProgram:
            return "正在写入游戏";
        case Cnmt::ContentTypeData:
            return "正在写入其数据";
        case Cnmt::ContentTypeControl:
            return "正在写入其图标与名称";
        case Cnmt::ContentTypeHtmlDocument:
            return "正在写入其手册";
        case Cnmt::ContentTypeLegalInformation:
            return "正在写入其法律声明";
        default:
            return "正在写入其他部分";
    }
}

NcaInfo TitleConverter::writeContent(TitleOutput& output, const ContentPair& content, bool& compressed) {
    NcaFile& primary = content.primary();
    std::vector<int> order = content.sectionsInFileOrder();
    bool streamed = false;
    for (int i : order) {
        SectionSource source;
        content.sectionAt(i, source);
        streamed = streamed || plan(content, source, _options) == Plan::CompressRomFs;
    }
    const char* label = labelOf(content);
    bool sign = false;

    if (!streamed) {
        NcaSections sections(NcaFile::SectionCount);
        for (int i : order) {
            SectionSource source;
            content.sectionAt(i, source);
            sections[(size_t)i] = buildSection(content, source, sign);
        }
        i64 total = NcaWriter::totalSize(sections);
        Progress progress(label, total);
        NcaStream& stream = output.beginNca(label, total, ".nca");
        NcaInfo info = _writer->write(stream, primary.contentType(), primary.programId(), sections, sign, &progress, primary.contentIndex(), primary.sdkVersion());
        progress.done();
        output.endNca(info, ".nca");
        return info;
    }

    // What it takes to rebuild it comes first, and only what's left over can go on keeping it in memory. Better found out now than hours from now.
    i64 needed = 0;
    for (int i : order) {
        SectionSource source;
        content.sectionAt(i, source);
        if (plan(content, source, _options) == Plan::CompressRomFs)
            needed += memoryToRebuild(source.section->hashedBytes(), _options.blockSize);
    }
    if (needed + MemoryForTheRest > sys::workMemory())
        throw std::bad_alloc();

    compressed = true;
    NcaStream& stream = output.beginNca(label, -1, ".nca");
    NcaBuilder builder(*_writer, stream, std::max<i64>(_options.holdInMemory - needed, 0), _options.checkResult);
    for (int i : order) {
        SectionSource source;
        content.sectionAt(i, source);
        if (plan(content, source, _options) == Plan::CompressRomFs) {
            std::vector<KeptRange> kept;
            bool merged = source.original && source.section->hasIndirect();
            if (merged)
                kept = source.owner->keptInImage(*source.section, source.original);
            compressInto(builder, i, source.owner->openRomFsImage(*source.section, source.original), "正在压缩游戏数据", merged ? &kept : nullptr);
        } else {
            builder.add(i, buildSection(content, source, sign));
        }
    }
    NcaInfo info = builder.finish(primary.contentType(), primary.programId(), sign, primary.contentIndex(), primary.sdkVersion());
    output.endNca(info, ".nca");
    return info;
}

NcaInfo TitleConverter::writeProbe(TitleOutput& output, const SourcePtr& image, u64 programId, const char* label) {
    NcaStream& stream = output.beginNca(label, -1, ".nca");
    NcaBuilder builder(*_writer, stream, 0);
    compressInto(builder, 0, image, label);
    NcaInfo info = builder.finish(NcaContentType::Data, programId, false, 0, BuildSdkVersion);
    output.endNca(info, ".nca");
    return info;
}

namespace {

/// The size of a built content meta: its header, the extended header of an Application, the records and a digest of zeros.
int builtCnmtSize(int records) { return 0x20 + 0x10 + records * 0x38 + 0x20; }

Bytes readWholeFile(const std::string& path) {
    sys::File file(path, sys::File::Mode::Read);
    Bytes data((size_t)file.size());
    if (!data.empty())
        file.read(0, data.data(), data.size());
    return data;
}

/// The name and the rest stay as they are. What goes is the logo the game would otherwise have to show as its own.
Bytes patchedNacp(const ExtractedGame& game) {
    constexpr size_t LogoHandling = 0x30F1;
    Bytes nacp = readWholeFile(game.folder + "/control/control.nacp");
    nacp[LogoHandling] = 0;
    return nacp;
}

}  // namespace

static Bytes patchNpdm(Bytes npdm, const std::string& what) {
    if (npdm.size() < 0x80 || memcmp(npdm.data(), "META", 4) != 0)
        throw ToolError(what + " 不是有效的 NPDM（META 魔数错误）");
    int aci0 = rdi32(&npdm[0x70]);
    int acid = rdi32(&npdm[0x78]);
    if (aci0 < 0 || (size_t)aci0 + 0x18 > npdm.size() || memcmp(&npdm[(size_t)aci0], "ACI0", 4) != 0)
        throw ToolError(what + " 不是有效的 NPDM（ACI0 区段错误）");
    if (acid < 0 || (size_t)acid + 0x204 > npdm.size() || memcmp(&npdm[(size_t)acid + 0x200], "ACID", 4) != 0)
        throw ToolError(what + " 不是有效的 NPDM（ACID 区段错误）");
    memcpy(&npdm[(size_t)acid + 0x100], signingModulus(), 0x100);
    return npdm;
}

namespace {

/// The control data's files as a copy has them: whatever it had of the map and the mark before goes, and this one's go in. The estimate builds
/// the same with nothing in the files, as only their sizes count there.
std::map<std::string, Bytes> controlFiles(const RomFsArchive& archive, const SourcePtr& image, const UpdateMap* map, const CopyMark* mark) {
    std::map<std::string, Bytes> files;
    for (auto& file : archive.files) {
        if (file.path == UpdateMap::FileName || file.path == CopyMark::FileName)
            continue;
        Bytes data((size_t)file.size);
        if (image)
            image->read(file.offset, data.data(), data.size());
        files[file.path] = std::move(data);
    }
    if (map)
        files[UpdateMap::FileName] = map->toBytes();
    if (mark)
        files[CopyMark::FileName] = mark->toBytes();
    return files;
}

}  // namespace

bool TitleConverter::controlWithNotes(const SectionSource& source, NcaSection& out) {
    SourcePtr image = source.owner->openRomFsImage(*source.section, source.original);
    RomFsArchive archive = RomFsArchive::open(image, format("%s section %d", source.owner->name().c_str(), source.section->index));
    out = Sections::romFs(buildRomFsImageOf(controlFiles(archive, image, _hasMap ? &_map : nullptr, _hasMark ? &_mark : nullptr), archive.directories), true, nullptr);
    return true;
}

NcaSection TitleConverter::buildSection(const ContentPair& content, const SectionSource& source, bool& sign) {
    NcaFile& owner = *source.owner;
    const NcaFsSection& section = *source.section;
    NcaSection control;
    if ((_hasMap || _hasMark) && content.type == Cnmt::ContentTypeControl && section.fsType() == NcaFsSection::FsRomFs && controlWithNotes(source, control))
        return control;
    switch (plan(content, source, _options)) {
        case Plan::RebuildRomFs: {
            SourcePtr image = owner.openRomFsImage(section, source.original);
            if (image->length() <= _options.keepSmallSections)
                image = std::make_shared<WholeInMemory>(image);
            Progress progress("正在校验游戏数据", image->length());
            NcaSection plain = Sections::romFs(image, true, &progress);
            progress.done();
            return plain;
        }

        case Plan::RebuildPfs0: {
            bool exefs = isExefs(content, section);
            auto archive = owner.openPfs0(section, source.original, true, _options.keepSmallSections);
            std::vector<std::pair<std::string, SourcePtr>> files;
            for (auto& file : archive->files()) {
                if (file.name == "main.npdm" && exefs && !_options.noSign) {
                    sign = true;
                    files.emplace_back(file.name, bytesSource(patchNpdm(archive->readAll(file), owner.name() + " main.npdm")));
                } else {
                    files.emplace_back(file.name, archive->open(file));
                }
            }
            return Sections::pfs0(buildPfs0(files), archive->hashBlockSize(), true);
        }

        default:
            return passThrough(source);
    }
}

void TitleConverter::compressInto(NcaBuilder& builder, int index, const SourcePtr& image, const char* label, const std::vector<KeptRange>* kept) {
    Progress progress(label, image->length());
    CompressedData data;
    builder.addCompressed(index, image, _options.blockSize, _options.level, _options.threads, true, progress, &data, kept);
    progress.done();
    sink().message(format("Game data of %s stored as %s (%.1f%%)", formatSize(data.virtualSize).c_str(), formatSize(data.dataSize).c_str(), data.dataSize * 100.0 / (double)std::max<i64>(data.virtualSize, 1)));
    if (data.sharedBytes > 0)
        sink().message(format("%s of it is held more than once, stored once", formatSize(data.sharedBytes).c_str()));
    _foundBytes += data.foundBytes;
}

NcaInfo TitleConverter::writeFolderNca(TitleOutput& output, const char* label, NcaContentType type, const std::string& folder, u64 titleId, const std::map<std::string, Bytes>& replacements) {
    NcaSections sections(NcaFile::SectionCount);
    sections[0] = Sections::romFs(buildRomFsImage(folder, replacements), true, nullptr);
    NcaStream& stream = output.beginNca(label, NcaWriter::totalSize(sections), ".nca");
    NcaInfo info = _writer->write(stream, type, titleId, sections, false, nullptr, 0, BuildSdkVersion);
    output.endNca(info, ".nca");
    return info;
}

void TitleConverter::build(const ExtractedGame& game, TitleOutput& output) {
    NczSource::setSpoolFolder(_options.tempDir);
    u64 titleId = game.titleId;
    std::vector<ContentRecord> records;
    bool compressed = false;

    {
        const char* label = "正在写入游戏";
        Bytes npdm = readWholeFile(game.folder + "/exefs/main.npdm");
        if (!_options.noSign)
            npdm = patchNpdm(std::move(npdm), game.folder + "/exefs/main.npdm");
        NcaSection exefs = Sections::pfs0(buildPfs0FromFolder(game.folder + "/exefs", {{"main.npdm", npdm}}), Sections::ExefsHashBlockSize, true);
        std::optional<NcaSection> logo;
        if (game.hasLogo)
            logo = Sections::pfs0(buildPfs0FromFolder(game.folder + "/logo"), Sections::LogoHashBlockSize, false);

        NcaInfo info;
        if (game.hasRomFs && _options.compress) {
            compressed = true;
            SourcePtr image = game.romFsImage;
            i64 needed = memoryToRebuild(image->length(), _options.blockSize);
            if (needed + MemoryForTheRest > sys::workMemory())
                throw std::bad_alloc();
            NcaStream& stream = output.beginNca(label, -1, ".nca");
            NcaBuilder builder(*_writer, stream, std::max<i64>(_options.holdInMemory - needed, 0), _options.checkResult);
            builder.add(0, std::move(exefs));
            compressInto(builder, 1, image, "正在压缩游戏数据");
            if (logo)
                builder.add(2, std::move(*logo));
            info = builder.finish(NcaContentType::Program, titleId, !_options.noSign, 0, BuildSdkVersion);
        } else {
            NcaSections sections(NcaFile::SectionCount);
            sections[0] = std::move(exefs);
            sections[2] = std::move(logo);
            if (game.hasRomFs) {
                SourcePtr image = game.romFsImage;
                Progress progress("正在校验游戏数据", image->length());
                sections[1] = Sections::romFs(image, true, &progress);
                progress.done();
            }
            i64 total = NcaWriter::totalSize(sections);
            Progress progress(label, total);
            NcaStream& stream = output.beginNca(label, total, ".nca");
            info = _writer->write(stream, NcaContentType::Program, titleId, sections, !_options.noSign, &progress, 0, BuildSdkVersion);
            progress.done();
        }
        output.endNca(info, ".nca");
        records.push_back(ContentRecord{info, Cnmt::ContentTypeProgram, 0});
    }

    records.push_back(ContentRecord{writeFolderNca(output, "正在写入其图标与名称", NcaContentType::Control, game.folder + "/control", titleId, {{"control.nacp", patchedNacp(game)}}), Cnmt::ContentTypeControl, 0});
    if (game.hasHtmlDoc)
        records.push_back(ContentRecord{writeFolderNca(output, "正在写入其手册", NcaContentType::Manual, game.folder + "/htmldoc", titleId, {}), Cnmt::ContentTypeHtmlDocument, 0});
    if (game.hasLegalInfo)
        records.push_back(ContentRecord{writeFolderNca(output, "正在写入其法律声明", NcaContentType::Manual, game.folder + "/legalinfo", titleId, {}), Cnmt::ContentTypeLegalInformation, 0});

    Bytes cnmt((size_t)builtCnmtSize((int)records.size()));
    u8* c = cnmt.data();
    wr64(c, titleId);
    wr32(c + 8, game.version);
    c[0x0C] = Cnmt::MetaApplication;
    wr16(c + 0x0E, 0x10);
    wr16(c + 0x10, (u16)records.size());
    wr64(c + 0x20, titleId + 0x800);
    wr32(c + 0x28, compressed ? CompressionMinSystemVersion : 0);
    for (size_t i = 0; i < records.size(); i++) {
        u8* r = c + 0x30 + i * 0x38;
        memcpy(r, records[i].nca.hash, 32);
        memcpy(r + 0x20, records[i].nca.hash, 16);
        i64 size = records[i].nca.size;
        for (int b = 0; b < 6; b++)
            r[0x30 + b] = (u8)(size >> (8 * b));
        r[0x36] = records[i].type;
        r[0x37] = records[i].idOffset;
    }

    NcaSections sections(1);
    sections[0] = Sections::pfs0(buildPfs0({{Cnmt::fileName(titleId, Cnmt::MetaApplication), bytesSource(cnmt)}}), Sections::MetaHashBlockSize, true);
    NcaStream& stream = output.beginNca("Meta", NcaWriter::totalSize(sections), ".cnmt.nca");
    NcaInfo metaInfo = _writer->write(stream, NcaContentType::Meta, titleId, sections, false, nullptr, 0, BuildSdkVersion);
    output.endNca(metaInfo, ".cnmt.nca");
    output.finish(ContentMeta::parse(cnmt, "构建出的内容元数据"), metaInfo);
}

/// The section's decrypted bytes are kept exactly as they are, so its hash tree stays valid.
NcaSection TitleConverter::passThrough(const SectionSource& source) {
    NcaFile& owner = *source.owner;
    const NcaFsSection& section = *source.section;
    if (section.fsType() == NcaFsSection::FsPartition) {
        owner.openPfs0(section, nullptr);
    } else if (_options.checkResult) {
        SourcePtr image = owner.openRomFsImage(section, nullptr);
        Progress progress("正在校验其他部分", image->length());
        Bytes buffer(1 << 20);
        for (i64 done = 0; done < image->length();) {
            size_t n = (size_t)std::min<i64>((i64)buffer.size(), image->length() - done);
            image->read(done, buffer.data(), n);
            done += (i64)n;
            progress.report(done, done);
        }
        progress.done();
    }

    NcaSection out;
    out.fsHeader = section.header;
    bool keepEncrypted = section.encryption() != NcaFsSection::EncryptNone;
    if (!keepEncrypted)
        out.fsHeader[4] = NcaFsSection::EncryptNone;
    out.data = owner.openPhysical(section);
    out.encrypted = keepEncrypted;
    out.upperIv = section.upperIv();
    return out;
}

// -------------------------------------------------------------------------------------------------------------------
// Extracting

namespace {

std::string targetPath(const std::string& folder, const std::string& relative, std::set<std::string>& seen) {
    std::string path = folder;
    size_t start = 0;
    while (start <= relative.size()) {
        size_t slash = relative.find('/', start);
        if (slash == std::string::npos)
            slash = relative.size();
        path += "/" + LocalNames::forFileSystem(relative.substr(start, slash - start));
        start = slash + 1;
    }
    std::string lower = path;
    for (char& c : lower)
        c = (char)tolower((u8)c);
    if (!seen.insert(lower).second)
        throw ToolError("'" + relative + "' would overwrite another file on this file system, whose names ignore case or can't hold every character");
    return path;
}

void copyOut(DataSource& source, i64 offset, i64 size, const std::string& path, u8* buffer, size_t bufferSize, Progress* progress, i64 doneBefore) {
    sys::File output(path, sys::File::Mode::Sized, size);
    for (i64 done = 0; done < size;) {
        checkStop();
        size_t n = (size_t)std::min<i64>((i64)bufferSize, size - done);
        source.read(offset + done, buffer, n);
        output.write(done, buffer, n);
        done += (i64)n;
        if (progress)
            progress->report(doneBefore + done, doneBefore + done);
    }
}

/// Read from the game data in one go, and shared among the files written from it.
struct Block {
    Bytes data;
    std::atomic<size_t> left{0};
};

/// A file large enough to be written a piece at a time, by whichever writers get to its pieces.
struct LargeOutput {
    std::string path;
    i64 size = 0;
    sys::Mutex mutex;
    std::unique_ptr<sys::File> file;
    std::atomic<int> left{0};
};

/// A small file, or a piece of a large one, and where it is in the block it's read into.
struct OutputItem {
    std::shared_ptr<Block> block;
    size_t at = 0;
    i64 size = 0;
    std::string path;
    std::shared_ptr<LargeOutput> large;
    i64 fileOffset = 0;
};

/// What's read in one go: a piece of a large file, or small files next to each other.
struct Span {
    i64 offset = 0, length = 0;
    std::vector<OutputItem> items;
};

/// Small files, which there can be hundreds of thousands of, are read together in batches, and large ones in pieces.
std::vector<Span> spansFor(const std::vector<RomFsArchive::Entry>& files, const std::string& folder, std::set<std::string>& seen) {
    Progress preparing("正在准备其文件", (i64)files.size());
    constexpr i64 LargeFile = 1 << 20;
    constexpr i64 PieceBytes = 8 << 20;
    constexpr i64 BatchBytes = 16 << 20;

    std::vector<Span> spans;
    for (size_t index = 0; index < files.size();) {
        preparing.report((i64)index);
        const RomFsArchive::Entry& first = files[index];
        if (first.size >= LargeFile) {
            auto large = std::make_shared<LargeOutput>();
            large->path = targetPath(folder, first.path, seen);
            large->size = first.size;
            large->left = (int)((first.size + PieceBytes - 1) / PieceBytes);
            for (i64 done = 0; done < first.size; done += PieceBytes) {
                Span span;
                span.offset = first.offset + done;
                span.length = std::min(PieceBytes, first.size - done);
                OutputItem item;
                item.size = span.length;
                item.large = large;
                item.fileOffset = done;
                span.items.push_back(std::move(item));
                spans.push_back(std::move(span));
            }
            index++;
            continue;
        }
        Span span;
        span.offset = first.offset;
        i64 end = first.offset;
        for (; index < files.size() && files[index].size < LargeFile && files[index].offset + files[index].size - first.offset <= BatchBytes; index++) {
            const RomFsArchive::Entry& file = files[index];
            OutputItem item;
            item.at = (size_t)(file.offset - first.offset);
            item.size = file.size;
            item.path = targetPath(folder, file.path, seen);
            span.items.push_back(std::move(item));
            end = std::max(end, file.offset + file.size);
        }
        span.length = end - first.offset;
        spans.push_back(std::move(span));
    }
    return spans;
}

/// Goes through a game's files with everything there is. Threads on every core read, decrypt and check the game data as far ahead as memory allows, and
/// more threads than that write it out, as most of what they do is wait for the file service, and creating a file takes far longer than what's in it.
void copyAllOut(DataSource& image, const std::vector<RomFsArchive::Entry>& files, const std::string& folder, std::set<std::string>& seen, Progress& progress) {
    constexpr i64 LeastAhead = 128LL << 20, MostAhead = 1LL << 30;
    std::vector<Span> spans = spansFor(files, folder, seen);

    sys::Mutex mutex;
    sys::CondVar changed;
    std::deque<OutputItem> queue;
    i64 ahead = 0, written = 0, filesWritten = 0;
    bool readingDone = false;
    std::exception_ptr failure;
    i64 mostAhead = std::max(LeastAhead, std::min(MostAhead, sys::spareMemory()));

    auto fail = [&](std::exception_ptr error) {
        sys::Lock lock(mutex);
        if (!failure)
            failure = error;
        changed.wakeAll();
    };

    auto writeOne = [&](OutputItem& item) {
        if (item.large) {
            sys::File* output;
            {
                sys::Lock lock(item.large->mutex);
                if (!item.large->file)
                    item.large->file.reset(new sys::File(item.large->path, sys::File::Mode::Sized, item.large->size));
                output = item.large->file.get();
            }
            output->write(item.fileOffset, item.block->data.data(), (size_t)item.size);
            if (--item.large->left == 0) {
                sys::Lock lock(item.large->mutex);
                item.large->file.reset();
            }
        } else {
            sys::File output(item.path, sys::File::Mode::Sized, item.size);
            output.write(0, item.block->data.data() + item.at, (size_t)item.size);
        }
    };

    std::vector<std::unique_ptr<sys::Thread>> writers;
    auto endWriters = [&] {
        {
            sys::Lock lock(mutex);
            readingDone = true;
            changed.wakeAll();
        }
        for (auto& writer : writers)
            writer->join();
    };
    try {
        for (int i = 0; i < sys::FileThreads; i++) {
            writers.emplace_back(new sys::Thread);
            writers.back()->start([&, i] {
                try {
                    while (true) {
                        OutputItem item;
                        {
                            sys::Lock lock(mutex);
                            while (queue.empty() && !readingDone && !failure)
                                changed.wait(mutex);
                            if (failure || queue.empty())
                                return;
                            item = std::move(queue.front());
                            queue.pop_front();
                        }
                        checkStop();
                        writeOne(item);
                        sys::Lock lock(mutex);
                        written += item.size;
                        progress.report(written, written);
                        if (!item.large || item.fileOffset + item.size >= item.large->size)
                            sink().files(++filesWritten, (i64)files.size());
                        if (--item.block->left == 0) {
                            ahead -= (i64)item.block->data.size();
                            changed.wakeAll();
                        }
                    }
                } catch (...) {
                    fail(std::current_exception());
                }
            }, sys::workerCore(i), 1 << 18, true);
        }
    } catch (...) {
        // Whichever ones did start are waiting to be told there's nothing more coming.
        endWriters();
        throw;
    }

    try {
        parallelFor(image.readsAtOnce() ? sys::workerCount() : 1, (int)spans.size(), [&](int index, int) {
            checkStop();
            Span& span = spans[(size_t)index];
            {
                sys::Lock lock(mutex);
                while (ahead > 0 && ahead + span.length > mostAhead && !failure)
                    changed.wait(mutex);
                if (failure)
                    return;
                ahead += span.length;
            }
            auto block = std::make_shared<Block>();
            block->data.resize((size_t)span.length);
            block->left = span.items.size();
            if (span.length > 0)
                image.read(span.offset, block->data.data(), (size_t)span.length);
            sys::Lock lock(mutex);
            for (auto& item : span.items) {
                item.block = block;
                queue.push_back(std::move(item));
            }
            changed.wakeAll();
        });
    } catch (...) {
        fail(std::current_exception());
    }
    endWriters();
    if (failure)
        std::rethrow_exception(failure);
}

std::string folderFor(const ContentPair& content, int section) {
    std::string name;
    switch (content.type) {
        case Cnmt::ContentTypeProgram:
            name = section == 0 ? "exefs" : section == 1 ? "romfs" : section == 2 ? "logo" : format("section%d", section);
            break;
        case Cnmt::ContentTypeData:
            name = "data";
            break;
        case Cnmt::ContentTypeControl:
            name = "control";
            break;
        case Cnmt::ContentTypeHtmlDocument:
            name = "htmldoc";
            break;
        case Cnmt::ContentTypeLegalInformation:
            name = "legalinfo";
            break;
        default:
            name = format("type%d", content.type);
            break;
    }
    if (content.type != Cnmt::ContentTypeProgram && section != 0)
        name += format("_section%d", section);
    if (content.idOffset == 0)
        return name;
    return content.type == Cnmt::ContentTypeProgram ? format("program%d/%s", content.idOffset, name.c_str()) : format("%s%d", name.c_str(), content.idOffset);
}

}  // namespace

std::string extractedFolderOf(const TitleJob& job, const TitleSet& set, const std::string& parent) {
    u32 version = job.update ? job.update->meta.version : job.main->meta.version;
    return parent + "/" + job.stem(version, set.sharesFile(job));
}

ExtractResult extractTitle(const TitleJob& job, const TitleSet& set, const std::string& parent) {
    constexpr size_t CopyBufferSize = 1 << 22;
    u32 version = job.update ? job.update->meta.version : job.main->meta.version;
    std::string root = parent + "/" + job.stem(version, set.sharesFile(job));
    sys::makeDirectories(root);
    // Said first, so that whatever happens part of the way through, the folder never looks like a whole game (ExtractedGame::NotFinished).
    std::string notFinished = root + "/" + ExtractedGame::NotFinished;
    {
        sys::File note(notFinished, sys::File::Mode::Create);
        note.write(0, ExtractedGame::NotFinishedText, strlen(ExtractedGame::NotFinishedText));
        note.flush();
    }
    NczSource::setSpoolFolder(parent);

    struct Release {
        const TitleJob& job;
        ~Release() { job.releaseMemory(); }
    } release{job};

    ExtractResult result;
    result.path = root;
    std::unique_ptr<u8[]> buffer(new u8[CopyBufferSize]);
    for (auto& content : job.contents) {
        for (int i : content.sectionsInFileOrder()) {
            SectionSource source = {};
            content.sectionAt(i, source);
            std::string folder = root + "/" + folderFor(content, i);
            sys::makeDirectories(folder);
            std::set<std::string> seen;

            if (source.section->fsType() == NcaFsSection::FsPartition) {
                auto archive = source.owner->openPfs0(*source.section, source.original);
                i64 total = 0;
                for (auto& file : archive->files())
                    total += file.size;
                // Said as the game data is, as a game can keep most of itself in its program.
                Progress progress(isExefs(content, *source.section) ? "正在解包其程序" : "正在解包其他部分", total);
                i64 done = 0;
                for (auto& file : archive->files()) {
                    copyOut(*archive->data(), file.offset, file.size, targetPath(folder, file.name, seen), buffer.get(), CopyBufferSize, &progress, done);
                    done += file.size;
                    result.bytes += file.size;
                    result.files++;
                }
                progress.done();
                continue;
            }

            sink().progress("正在读取其文件列表", 0, 1, 0);
            SourcePtr image = source.owner->openRomFsImage(*source.section, source.original);
            RomFsArchive archive = RomFsArchive::open(image, source.owner->name());
            sink().message(format("%s: %d folders and %d files", folderFor(content, i).c_str(), (int)archive.directories.size(), (int)archive.files.size()));
            // A level at a time, so that what each one goes in is there, and there's no need to go making sure of it.
            std::vector<std::vector<std::string>> levels;
            for (auto& directory : archive.directories) {
                std::string path = folder;
                size_t start = 0;
                while (start <= directory.size()) {
                    size_t slash = directory.find('/', start);
                    if (slash == std::string::npos)
                        slash = directory.size();
                    path += "/" + LocalNames::forFileSystem(directory.substr(start, slash - start));
                    start = slash + 1;
                }
                size_t depth = (size_t)std::count(directory.begin(), directory.end(), '/');
                if (levels.size() <= depth)
                    levels.resize(depth + 1);
                levels[depth].push_back(std::move(path));
            }
            Progress making("正在创建其文件夹", (i64)archive.directories.size());
            sys::Mutex makingLock;
            i64 made = 0;
            for (auto& level : levels) {
                parallelFor(sys::FileThreads, (int)level.size(), [&](int index, int) {
                    checkStop();
                    sys::makeDirectory(level[(size_t)index]);
                    sys::Lock lock(makingLock);
                    making.report(++made);
                });
            }
            making.done();

            i64 total = 0;
            for (auto& file : archive.files)
                total += file.size;
            // Its own name for each part, as each of them starts again from nothing.
            Progress progress(content.type == Cnmt::ContentTypeProgram ? "正在解包游戏数据" : "正在解包其他部分", total);

            // In image order, so a hard drive reads steadily and each hash block is only checked once.
            std::vector<RomFsArchive::Entry> ordered = archive.files;
            std::stable_sort(ordered.begin(), ordered.end(), [](auto& a, auto& b) { return a.offset < b.offset; });
            copyAllOut(*image, ordered, folder, seen, progress);
            progress.done();
            result.bytes += total;
            result.files += (i64)ordered.size();
        }
    }
    // All of it's out. A card that turns taking the note away down is asked again a moment later: with it there, the game isn't listed.
    for (int tries = 1; !sys::removeFile(notFinished) && sys::presence(notFinished) != sys::Presence::Missing; tries++) {
        if (tries >= 4)
            throw ToolError("'" + notFinished + "' couldn't be deleted, and while it's there, the game isn't taken as all there. Extract it again in a moment");
        sys::sleepMs(1000 * tries);
    }
    sys::commit(root);
    return result;
}

// -------------------------------------------------------------------------------------------------------------------
// Estimating

namespace {

constexpr int RegionSize = 0x10000;
/// Enough to time reading with, one thread at a time and then all of them at once.
constexpr int TrialRegions = 24;
constexpr int BatchRegions = 128;
constexpr int MinRegions = 128;
/// However small an image is, this much of it gets a look. Anything smaller just gets read.
constexpr int SmallestSample = 32;
constexpr int MaxRegions = 8192;
constexpr double TimeBudgetSeconds = 1;
constexpr double GoodEnoughMargin = 0.02;
/// It has to get this much more read before it's worth having every thread read at once.
constexpr double TogetherHasToBeat = 1.2;
/// A read that takes this long means a hard drive hunting for it, and asking for more at once would only have it hunt more.
constexpr double SeekingSeconds = 0.004;
constexpr double GoldenRatioConjugate = 0.6180339887498949;
constexpr double Confidence95 = 1.96;

struct Tally {
    i64 romFs = 0;
    i64 stored = 0;
    double variance = 0;
};

struct RegionResult {
    i64 region;
    int length;
    int stored;
    int lz4Blocks;
    int rawBlocks;
    /// What of it was compressed, which is all of it but any piece that's held again.
    int counted;
};

struct SampleResult {
    i64 stored = 0;
    double margin = 0;
    i64 lz4Blocks = 0;
    i64 rawBlocks = 0;
};

i64 greatestCommonDivisor(i64 a, i64 b) {
    while (b != 0) {
        i64 next = a % b;
        a = b;
        b = next;
    }
    return a;
}

/// A later copy of a file the file table shows, where it is, and how much of it compressing finds held before.
struct FileCopy {
    i64 offset = 0;
    i64 length = 0;
    i64 held = 0;
    bool nearCopy = false;
};

/// What the file table shows a game holds again: later copies of its files, whole or nearly so, which compressing finds and stores once, as
/// FileCopies does on the PC, which says why each of these is what it is. It reads what the PC reads, in the same order, so both come to the
/// same copies within the same number of reads.
namespace copies {
constexpr i64 Smallest = 1 << 20;
constexpr int Piece = 0x1000;
constexpr int LaterEnd = 0x10000;
constexpr int EarlierEnd = 0x40000;
constexpr double EndsAlike = 0.8;
constexpr size_t FewestSpots = 4;
constexpr double CloseInSize = 0.2;
constexpr double VersionsSpread = 1.2;
constexpr double NearHeld = 0.95;
constexpr int MostReads = 128;
constexpr int FirstReads = 16;
constexpr double TimeShare = 0.25;

using Files = std::vector<RomFsArchive::Entry>;

/// Groups in the order their first file comes, each key worked out once, in order, as LINQ's GroupBy has them on the PC.
template <typename Key>
std::vector<Files> groupBy(const Files& files, Key key) {
    std::vector<Files> groups;
    std::map<decltype(key(files.front())), size_t> index;
    for (auto& file : files) {
        auto k = key(file);
        auto known = index.find(k);
        if (known == index.end()) {
            index.emplace(k, groups.size());
            groups.push_back({file});
        } else {
            groups[known->second].push_back(file);
        }
    }
    groups.erase(std::remove_if(groups.begin(), groups.end(), [](const Files& g) { return g.size() < 2; }), groups.end());
    return groups;
}

std::string nameOf(const std::string& path) {
    size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

/// What of a file the update's map already has held again: those places are stored once without any of this.
i64 mapBytes(const std::vector<i64>& repeats, int blockSize, const RomFsArchive::Entry& file) {
    i64 end = file.offset + file.size, total = 0;
    for (auto it = std::lower_bound(repeats.begin(), repeats.end(), file.offset - blockSize + 1); it != repeats.end() && *it < end; ++it)
        total += std::min<i64>(*it + blockSize, end) - std::max<i64>(*it, file.offset);
    return total;
}

/// A group of files the same size, or a later file with the same name as an earlier one, and how much it could find held again.
struct Check {
    i64 potential = 0;
    i64 offset = 0;
    const Files* group = nullptr;
    bool named = false;
    RomFsArchive::Entry later{};
    RomFsArchive::Entry earlier{};
};

std::vector<FileCopy> find(const SourcePtr& image, const std::vector<i64>& repeats, int blockSize, double seconds) {
    double started = sys::now();
    std::vector<FileCopy> found;
    Files files;
    try {
        for (auto& file : RomFsArchive::open(image, "The game data").files) {
            if (file.size >= Smallest)
                files.push_back(file);
        }
    } catch (const ToolError&) {
        // Compressing will say what's wrong with it.
        return found;
    }
    std::stable_sort(files.begin(), files.end(), [](const RomFsArchive::Entry& a, const RomFsArchive::Entry& b) { return a.offset < b.offset; });
    // Two files in the same place are the same bytes, stored once already.
    files.erase(std::unique(files.begin(), files.end(), [](const RomFsArchive::Entry& a, const RomFsArchive::Entry& b) { return a.offset == b.offset; }), files.end());

    // Every check there is to make, the one that could find the most first, so a game with many files the same size still gets to its
    // biggest copies within the reads allowed.
    auto sizes = groupBy(files, [](const RomFsArchive::Entry& f) { return f.size; });
    auto names = groupBy(files, [](const RomFsArchive::Entry& f) { return nameOf(f.path); });
    std::vector<Check> checks;
    for (auto& group : sizes) {
        Check check;
        check.potential = group[0].size * (i64)(group.size() - 1);
        check.offset = group[0].offset;
        check.group = &group;
        checks.push_back(check);
    }
    for (auto& group : names) {
        if (group.size() >= 3) {
            i64 most = group[0].size, least = group[0].size;
            for (auto& f : group) {
                most = std::max(most, f.size);
                least = std::min(least, f.size);
            }
            if ((double)most > VersionsSpread * (double)least)
                continue;
        }
        for (size_t i = 1; i < group.size(); i++) {
            const RomFsArchive::Entry& later = group[i];
            size_t closest = 0;
            for (size_t j = 1; j < i; j++) {
                if (std::llabs(group[j].size - later.size) < std::llabs(group[closest].size - later.size))
                    closest = j;
            }
            const RomFsArchive::Entry& earlier = group[closest];
            if ((double)std::llabs(later.size - earlier.size) > CloseInSize * (double)std::max(later.size, earlier.size))
                continue;
            Check check;
            check.potential = std::min(later.size, earlier.size);
            check.offset = later.offset;
            check.group = &group;
            check.named = true;
            check.later = later;
            check.earlier = earlier;
            checks.push_back(check);
        }
    }
    std::stable_sort(checks.begin(), checks.end(), [](const Check& a, const Check& b) { return a.potential != b.potential ? a.potential > b.potential : a.offset < b.offset; });

    int reads = 0;
    Bytes buffer((size_t)EarlierEnd);
    auto pieceAt = [&](i64 at, u64 seed) {
        image->read(at, buffer.data(), (size_t)Piece);
        reads++;
        return RomFsCompressor::blockHash(buffer.data(), (size_t)Piece, seed);
    };

    // An end is read once, however many files it's compared with.
    std::map<std::pair<i64, int>, std::vector<u64>> spotsRead;
    auto spotsAt = [&](i64 at, int length) -> const std::vector<u64>& {
        auto key = std::make_pair(at, length);
        auto known = spotsRead.find(key);
        if (known != spotsRead.end())
            return known->second;
        image->read(at, buffer.data(), (size_t)length);
        reads++;
        return spotsRead[key] = RomFsCompressor::spotsIn(buffer.data(), (size_t)length, blockSize);
    };
    auto endsAlike = [&](const RomFsArchive::Entry& later, const RomFsArchive::Entry& earlier, bool end) {
        int mine = (int)std::min<i64>(LaterEnd, later.size), theirs = (int)std::min<i64>(EarlierEnd, earlier.size);
        const std::vector<u64>& spots = spotsAt(end ? later.offset + later.size - mine : later.offset, mine);
        if (spots.size() < FewestSpots)
            return false;
        const std::vector<u64>& against = spotsAt(end ? earlier.offset + earlier.size - theirs : earlier.offset, theirs);
        size_t common = 0;
        for (u64 spot : spots)
            common += std::binary_search(against.begin(), against.end(), spot) ? 1 : 0;
        return (double)common >= EndsAlike * (double)spots.size();
    };

    // Before every read, as one group of files the same size can take many: once the time's up, what's left is left out.
    auto outOfTime = [&] { return reads >= FirstReads && sys::now() - started > seconds; };

    // Where each later copy is in the list, so a near copy that turns out to be a whole one is counted as that instead.
    std::map<i64, size_t> copied;
    for (auto& check : checks) {
        if (outOfTime())
            break;
        if (!check.named) {
            // The same size, then the same 4 KB from the middle, then from the start and the end: a copy of the whole file.
            const Files& group = *check.group;
            if (reads + (int)group.size() > MostReads)
                continue;
            std::map<const RomFsArchive::Entry*, u64> middle;
            for (auto& file : group) {
                if (outOfTime())
                    return found;
                middle[&file] = pieceAt(file.offset + file.size / 2 / Piece * Piece, 0);
            }
            auto middles = groupBy(group, [&](const RomFsArchive::Entry& f) { return middle[&f]; });
            for (auto& alike : middles) {
                if (reads + 2 * (int)alike.size() > MostReads)
                    break;
                std::map<const RomFsArchive::Entry*, u64> both;
                for (auto& file : alike) {
                    if (outOfTime())
                        return found;
                    u64 start = pieceAt(file.offset, 0);
                    both[&file] = start ^ pieceAt(file.offset + file.size - Piece, 1);
                }
                auto ends = groupBy(alike, [&](const RomFsArchive::Entry& f) { return both[&f]; });
                for (auto& same : ends) {
                    for (size_t i = 1; i < same.size(); i++) {
                        FileCopy copy{same[i].offset, same[i].size, std::max<i64>(0, same[i].size - blockSize - mapBytes(repeats, blockSize, same[i])), false};
                        auto known = copied.find(same[i].offset);
                        if (known != copied.end()) {
                            found[known->second] = copy;
                            continue;
                        }
                        copied[same[i].offset] = found.size();
                        found.push_back(copy);
                    }
                }
            }
        } else {
            // The same name, about the same size, and the end or the start much the same as the earlier one's: a near copy.
            if (copied.count(check.later.offset) || reads + 4 > MostReads)
                continue;
            if (!endsAlike(check.later, check.earlier, true) && (outOfTime() || !endsAlike(check.later, check.earlier, false)))
                continue;
            i64 held = (i64)(NearHeld * (double)std::min(check.later.size, check.earlier.size)) - blockSize - mapBytes(repeats, blockSize, check.later);
            copied[check.later.offset] = found.size();
            found.push_back(FileCopy{check.later.offset, check.later.size, std::max<i64>(0, held), true});
        }
    }
    return found;
}
}  // namespace copies

/// A stretch of the image the sample leaves out, as it's stored somewhere else.
struct Skip {
    i64 start;
    i64 end;
};

/// The blocks the update's map has held again and the later copies of files, in order, any that touch made one.
std::vector<Skip> leftOut(const std::vector<i64>& repeats, int blockSize, const std::vector<FileCopy>& found) {
    std::vector<Skip> all;
    for (i64 repeat : repeats)
        all.push_back(Skip{repeat, repeat + blockSize});
    for (auto& copy : found)
        all.push_back(Skip{copy.offset, copy.offset + copy.length});
    std::stable_sort(all.begin(), all.end(), [](const Skip& a, const Skip& b) { return a.start < b.start; });
    std::vector<Skip> merged;
    for (auto& skip : all) {
        if (!merged.empty() && skip.start <= merged.back().end)
            merged.back().end = std::max(merged.back().end, skip.end);
        else
            merged.push_back(skip);
    }
    return merged;
}

/// counted is how much of the image is stored where it is: all of it but what's held again. How well that compresses comes only from what of
/// it the sample has, as what's held again can compress better or worse than the rest.
SampleResult summarize(const std::vector<RegionResult>& samples, i64 counted) {
    double sampled = 0, read = 0, storedTotal = 0;
    i64 lz4 = 0, raw = 0;
    for (auto& s : samples) {
        read += s.length;
        sampled += s.counted;
        storedTotal += s.stored;
        lz4 += s.lz4Blocks;
        raw += s.rawBlocks;
    }
    if (read == 0)
        throw ToolError("无法读取任何游戏数据用于采样");
    SampleResult result;
    // All it came to was pieces held again: what's left is taken as not compressing at all, give or take all of it.
    if (sampled == 0) {
        result.stored = counted;
        result.margin = (double)counted;
        return result;
    }
    double ratio = storedTotal / sampled;
    double scale = (double)counted / sampled;

    // Neighbouring regions tend to hold similar data, so differences between neighbours give the honest spread for an evenly
    // spread sample. An image small enough to read completely has none.
    std::vector<RegionResult> ordered;
    for (auto& s : samples) {
        if (s.counted > 0)
            ordered.push_back(s);
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](auto& a, auto& b) { return a.region < b.region; });
    double squares = 0;
    for (size_t i = 1; i < ordered.size(); i++) {
        double difference = ordered[i].stored / (double)ordered[i].counted - ordered[i - 1].stored / (double)ordered[i - 1].counted;
        squares += difference * difference;
    }
    double count = (double)ordered.size();
    double unsampled = std::max(0.0, 1 - sampled / (double)counted);
    double variance = count > 1 ? unsampled / count * squares / (2.0 * (count - 1)) : 0;

    result.stored = (i64)(ratio * (double)counted);
    result.margin = Confidence95 * sqrt(variance) * (double)counted;
    result.lz4Blocks = (i64)((double)lz4 * scale);
    result.rawBlocks = (i64)((double)raw * scale);
    return result;
}

/// Walks the image with a golden ratio stride, which stays evenly spread however soon it stops and reaches every region exactly
/// once. A slow disk gets the minimum sample, and a fast one a far larger sample in about the same time. A solid NCZ can only be
/// read from front to back, so that takes a single sweep, of however long the whole file takes.
///
/// Every thread keeps going between reading and compressing, so the processor is never waiting for the disk or the other way round.
/// Reads go in order, so a hard drive sweeps across rather than jumping back and forth, and one at a time, unless timing them shows
/// that reading several at once gets more read, which it does on what has no seek time to lose. Going by the time it takes means it
/// doesn't matter what a drive says it is, or what an adapter says on its behalf.
///
/// What's kept to the minimum on a slow disk is how many times it has to go looking. Reading more at each place doesn't make up for that, as
/// regions next to each other say much the same: on a hard drive, four in a row each time took as long as one, and narrowed the margin not at all.
///
/// skips are what's stored somewhere else, in order: where a piece of the original game is held again, and later copies of files. A region
/// counts nothing for what it has of one, however that would compress. counted is how much of the image is stored where it is, and spent the
/// time already taken looking for copies, out of the time allowed.
SampleResult sample(const SourcePtr& image, const ConvertOptions& options, bool inOrder, const std::vector<Skip>& skips, i64 counted, double spent) {
    i64 length = image->length();
    i64 regions = (length + RegionSize - 1) / RegionSize;
    if (regions == 0)
        return SampleResult{};

    i64 stride = (i64)((double)regions * GoldenRatioConjugate);
    while (greatestCommonDivisor(stride, regions) != 1)
        stride++;

    // A small image takes up less of the answer, and doesn't need as close a look.
    int minimum = (int)std::min<i64>(MinRegions, std::max<i64>(regions / 16, SmallestSample));
    double budget = std::max(0.0, TimeBudgetSeconds * std::min(1.0, (double)length / (1LL << 30)) - spent);

    const i64 reportedTotal = (i64)minimum * RegionSize;
    Progress progress("正在对游戏数据采样", reportedTotal);
    double started = sys::now();
    std::vector<RegionResult> samples;
    i64 position = (i64)((u64)length ^ (u64)length >> 32) & 0x7FFFFFFF;

    // Done is whichever is further off: reaching the minimum sample, or the end of the time allowed.
    sys::Mutex reporting;
    std::atomic<int> regionsRead{0};
    auto report = [&](int read) {
        double done = std::min(std::min((double)read / minimum, budget > 0 ? (sys::now() - started) / budget : 1.0), 1.0);
        sys::Lock lock(reporting);
        progress.report((i64)(done * reportedTotal));
    };

    std::vector<std::unique_ptr<Lz4Encoder>> encoders;
    std::vector<std::unique_ptr<u8[]>> scratch;
    std::vector<std::unique_ptr<u8[]>> buffers;
    for (int i = 0; i < options.threads; i++) {
        encoders.emplace_back(new Lz4Encoder);
        scratch.emplace_back(new u8[Lz4Encoder::MaxBlockSize]);
        buffers.emplace_back(new u8[RegionSize]);
    }

    // What's around a piece held again is compressed in blocks from where it starts, as the compressor cuts them.
    auto compressRegion = [&](const u8* data, int regionLength, i64 region, int worker) {
        int stored = 0, lz4 = 0, raw = 0, counted = 0;
        i64 offset = region * RegionSize;
        auto skip = std::partition_point(skips.begin(), skips.end(), [&](const Skip& s) { return s.end <= offset; });
        for (int done = 0; done < regionLength;) {
            if (skip != skips.end() && skip->start <= offset + done) {
                done = (int)std::min<i64>(regionLength, skip->end - offset);
                ++skip;
                continue;
            }
            int until = skip != skips.end() ? (int)std::min<i64>(regionLength, skip->start - offset) : regionLength;
            int n = std::min(options.blockSize, until - done);
            int size = encoders[(size_t)worker]->compress(data + done, n, scratch[(size_t)worker].get(), RomFsCompressor::compressCapacity(n), options.level);
            if (size > 0) {
                stored += size + RomFsCompressor::AveragePadding;
                lz4++;
            } else {
                stored += n;
                raw++;
            }
            counted += n;
            done += n;
        }
        return RegionResult{region, regionLength, stored, lz4, raw, counted};
    };

    // A go at each way of reading, and then whichever got more done.
    bool canTryTogether = options.threads > 1 && image->readsAtOnce();
    enum class Stage { TimingOneAtATime, TimingTogether, Decided } stage = !inOrder && regions >= TrialRegions * 4 ? Stage::TimingOneAtATime : Stage::Decided;
    bool together = false;
    double oneAtATime = 0;

    struct EndPass {
        bool active;
        ~EndPass() {
            if (active)
                NczSource::endSinglePass();
        }
    } endPass{inOrder};
    report(0);
    if (inOrder)
        NczSource::beginSinglePass();

    SampleResult result;
    do {
        int wanted = inOrder ? minimum : stage != Stage::Decided ? TrialRegions : std::max(BatchRegions, minimum - (int)samples.size());
        int count = (int)std::min<i64>(wanted, regions - (i64)samples.size());
        std::vector<i64> chosen((size_t)count);
        for (int i = 0; i < count; i++) {
            chosen[(size_t)i] = position % regions;
            position = chosen[(size_t)i] + stride;
        }

        std::sort(chosen.begin(), chosen.end());
        std::vector<RegionResult> results((size_t)count);

        if (inOrder) {
            // A solid NCZ keeps track of how far it has got for the thread that's reading it, so all of it is read here first.
            Bytes input((size_t)count * RegionSize);
            std::vector<int> lengths((size_t)count);
            for (int i = 0; i < count; i++) {
                i64 offset = chosen[(size_t)i] * RegionSize;
                int n = (int)std::min<i64>(RegionSize, length - offset);

                // An update can jump back to earlier data in its base game. That region is left out rather than reading it all again.
                try {
                    image->read(offset, input.data() + (size_t)i * RegionSize, (size_t)n);
                    lengths[(size_t)i] = n;
                } catch (const NczRewind&) {
                }
                report((int)samples.size() + i + 1);
            }
            parallelFor(options.threads, count, [&](int i, int worker) { results[(size_t)i] = compressRegion(input.data() + (size_t)i * RegionSize, lengths[(size_t)i], chosen[(size_t)i], worker); });
        } else {
            bool atOnce = together || stage == Stage::TimingTogether;
            std::atomic<int> next{0};
            sys::Mutex gate;
            std::vector<double> reading((size_t)options.threads, 0.0);
            parallelFor(options.threads, count, [&](int, int worker) {
                u8* buffer = buffers[(size_t)worker].get();
                auto readNext = [&] {
                    int i = next.fetch_add(1);
                    i64 offset = chosen[(size_t)i] * RegionSize;
                    double begun = sys::now();
                    image->read(offset, buffer, (size_t)std::min<i64>(RegionSize, length - offset));
                    reading[(size_t)worker] += sys::now() - begun;
                    return i;
                };
                int i;
                if (atOnce) {
                    i = readNext();
                } else {
                    sys::Lock lock(gate);
                    i = readNext();
                }
                i64 offset = chosen[(size_t)i] * RegionSize;
                results[(size_t)i] = compressRegion(buffer, (int)std::min<i64>(RegionSize, length - offset), chosen[(size_t)i], worker);
                report(regionsRead.fetch_add(1) + 1);
            });

            double spent = 0;
            for (double seconds : reading)
                spent += seconds;
            double perRegion = std::max(spent / count, 1e-6);
            if (stage == Stage::TimingOneAtATime) {
                oneAtATime = perRegion;
                stage = canTryTogether && (perRegion < SeekingSeconds || options.noSeekTime) ? Stage::TimingTogether : Stage::Decided;
            } else if (stage == Stage::TimingTogether) {
                together = options.threads / perRegion > TogetherHasToBeat / oneAtATime;
                stage = Stage::Decided;
            }
        }
        for (auto& r : results) {
            if (r.length > 0)
                samples.push_back(r);
        }
        result = summarize(samples, counted);
        checkStop();
        if (samples.size() >= (size_t)std::min<i64>(regions, MaxRegions))
            break;
    } while (!inOrder && ((int)samples.size() < minimum || (sys::now() - started < budget && result.margin > (double)result.stored * GoodEnoughMargin)));

    image->release();
    progress.done();
    // What's only a few regions isn't worth a line of its own, and a game can have a lot of them.
    if (regions >= TrialRegions * 4)
        sink().message(format("Took %d samples of the game data in %.2f s, give or take %s, reading %s, %.1f ms each", (int)samples.size(), sys::now() - started, formatSize((i64)result.margin).c_str(), together ? "并发处理" : "逐个处理", oneAtATime * 1000));
    return result;
}

i64 compressedRomFsSize(const SourcePtr& image, const ConvertOptions& options, Tally& tally, bool inOrder, const RomFsCompressor::SharedPlan* shared = nullptr) {
    // What's held again is stored where it's held first, so the sample leaves it out wherever it's held again, and how well the rest
    // compresses goes for all of the rest alone, as what's held again can compress better or worse. Each place it's held again takes a
    // table entry of its own. That's what the update's map has held again, and the copies of files the file table shows: of those, only
    // what isn't held before is stored, and how well it compresses goes by the rest. A solid NCZ can only be read in order, so it has no
    // look for copies.
    double started = sys::now();
    std::vector<i64> repeats;
    if (shared) {
        for (auto& block : shared->blocks) {
            if (block.piece >= 0 && !block.first)
                repeats.push_back(block.offset);
        }
    }
    double seconds = copies::TimeShare * TimeBudgetSeconds * std::min(1.0, (double)image->length() / (1LL << 30));
    std::vector<FileCopy> found = inOrder ? std::vector<FileCopy>{} : copies::find(image, repeats, options.blockSize, seconds);
    i64 held = 0, nearHeld = 0;
    for (auto& copy : found) {
        held += copy.held;
        nearHeld += copy.nearCopy ? copy.held : 0;
    }
    if (!found.empty())
        sink().message(format("The file table shows %d copies of files, %s of them held again, %s of that in near copies, found in %.2f s", (int)found.size(),
                              formatSize(held).c_str(), formatSize(nearHeld).c_str(), sys::now() - started));
    i64 counted = std::max<i64>(0, image->length() - (i64)repeats.size() * options.blockSize - held);
    SampleResult s = sample(image, options, inOrder, leftOut(repeats, options.blockSize, found), counted, sys::now() - started);
    tally.romFs += image->length();
    tally.stored += s.stored;
    // How much of a near copy is held before is taken from what others came to, so it's give or take a little of it.
    double nearMargin = 0.05 * (double)nearHeld * (counted > 0 ? (double)s.stored / (double)counted : 1.0);
    tally.variance += s.margin * s.margin + nearMargin * nearMargin;

    // Raw blocks in a row share one table entry, so there can't be more raw entries than gaps between compressed blocks.
    i64 entries = s.lz4Blocks + std::min(s.rawBlocks, s.lz4Blocks + 1) + (i64)repeats.size() + held / options.blockSize;
    return Sections::compressedRomFsSectionSize(s.stored, (int)std::max<i64>(std::min<i64>(entries, INT32_MAX), 1));
}

/// All of the image read once, as compressing reads it, and what it comes to counted rather than written.
i64 measuredRomFsSize(const SourcePtr& image, const ConvertOptions& options, Tally& tally, const std::vector<KeptRange>* kept) {
    double started = sys::now();
    Progress progress("正在测量游戏数据", image->length());
    RomFsCompressor::Measurement measured = RomFsCompressor::measure(image, options.blockSize, options.level, options.threads, progress, kept, std::max(options.measureEvery, 1));
    progress.done();
    tally.romFs += image->length();
    tally.stored += measured.stored;
    // Kept from being fused into one multiply-add, which the PC doesn't do, so the margin comes out the same on both.
    volatile double square = measured.margin * measured.margin;
    tally.variance += square;
    sink().message(format("Measured %s of game data in %.1f s: %s of it held again, the rest comes to %s, give or take %s", formatSize(image->length()).c_str(),
                          sys::now() - started, formatSize(measured.heldBytes).c_str(), formatSize(measured.stored).c_str(), formatSize((i64)ceil(measured.margin)).c_str()));

    // Raw blocks in a row share one table entry, so there can't be more raw entries than gaps between compressed blocks.
    i64 entries = measured.lz4Blocks + std::min(measured.rawBlocks, measured.lz4Blocks + 1) + measured.held;
    return Sections::compressedRomFsSectionSize(measured.stored, (int)std::max<i64>(std::min<i64>(entries, INT32_MAX), 1));
}

i64 plainRomFsSize(i64 image, Tally& tally) {
    tally.romFs += image;
    tally.stored += image;
    return Sections::romFsSectionSize(image);
}

i64 sectionSize(const ContentPair& content, const SectionSource& source, const ConvertOptions& options, Tally& tally, bool measure) {
    NcaFile& owner = *source.owner;
    const NcaFsSection& section = *source.section;
    switch (TitleConverter::plan(content, source, options)) {
        case TitleConverter::Plan::CompressRomFs: {
            bool inOrder = owner.readsInOrder() || (source.original && source.original->readsInOrder());
            std::vector<KeptRange> kept;
            bool merged = source.original && section.hasIndirect();
            if (merged)
                kept = owner.keptInImage(section, source.original);
            if (measure)
                return measuredRomFsSize(owner.openRomFsImage(section, source.original), options, tally, merged ? &kept : nullptr);
            SourcePtr image = owner.openRomFsImage(section, source.original, false);
            RomFsCompressor::SharedPlan shared = RomFsCompressor::planShared(image->length(), options.blockSize, merged ? &kept : nullptr);
            return compressedRomFsSize(image, options, tally, inOrder, &shared);
        }
        case TitleConverter::Plan::RebuildRomFs:
            return plainRomFsSize(owner.openRomFsImage(section, source.original, false)->length(), tally);
        case TitleConverter::Plan::RebuildPfs0: {
            auto archive = owner.openPfs0(section, source.original, false);
            std::vector<std::pair<std::string, SourcePtr>> files;
            for (auto& file : archive->files())
                files.emplace_back(file.name, std::make_shared<ZeroSource>(file.size));
            return Sections::pfs0SectionSize(buildPfs0(files)->length(), archive->hashBlockSize());
        }
        default:
            return section.size;
    }
}

i64 metaNcaSize(const std::string& cnmtName, i64 cnmtSize) {
    SourcePtr pfs0 = buildPfs0({{cnmtName, std::make_shared<ZeroSource>(cnmtSize)}});
    return NcaWriter::HeaderRegionSize + NcaWriter::alignToSector(Sections::pfs0SectionSize(pfs0->length(), Sections::MetaHashBlockSize));
}

}  // namespace

namespace {

SizeEstimate workOut(const TitleJob& job, const ConvertOptions& given, bool measure) {
    ConvertOptions options = given;
    if (options.threads <= 0)
        options.threads = sys::workerCount();
    // Read as compressing reads it, which can need somewhere to put a solid NCZ's earlier data.
    if (measure)
        NczSource::setSpoolFolder(options.tempDir);

    struct Release {
        const TitleJob& job;
        ~Release() { job.releaseMemory(); }
    } release{job};

    Tally tally;
    i64 output = NspOutput::headerSize((int)job.contents.size());
    // A game made one with its update has the map of what the update kept in its control data, compressed or not, as a newer update is
    // made part of either with it, and a compressed copy has its mark there too: the control data is made again with them, as converting
    // makes it.
    UpdateMap map;
    bool hasMap = UpdateMap::forJob(job, options.compress, map);
    CopyMark mark;
    if (options.compress)
        mark = CopyMark::forJob(job, options);
    for (auto& content : job.contents) {
        i64 nca = NcaWriter::HeaderRegionSize;
        for (int i : content.sectionsInFileOrder()) {
            SectionSource source = {};
            content.sectionAt(i, source);
            if ((hasMap || options.compress) && content.type == Cnmt::ContentTypeControl && source.section->fsType() == NcaFsSection::FsRomFs) {
                SourcePtr image = source.owner->openRomFsImage(*source.section, source.original, false);
                RomFsArchive archive = RomFsArchive::open(image, format("%s section %d", source.owner->name().c_str(), source.section->index));
                nca += NcaWriter::alignToSector(Sections::romFsSectionSize(buildRomFsImageOf(
                    controlFiles(archive, nullptr, hasMap ? &map : nullptr, options.compress ? &mark : nullptr), archive.directories)->length()));
                continue;
            }
            nca += NcaWriter::alignToSector(sectionSize(content, source, options, tally, measure));
        }
        output += nca;
    }

    const ContentMeta& meta = job.main->meta;
    output += metaNcaSize(Cnmt::fileName(meta.titleId, meta.type), meta.rebuiltSize((int)job.contents.size()));

    SizeEstimate estimate;
    estimate.inputBytes = job.inputBytes;
    estimate.outputBytes = output;
    estimate.marginBytes = (i64)ceil(sqrt(tally.variance));
    estimate.romFsBytes = tally.romFs;
    estimate.romFsStoredBytes = tally.stored;
    estimate.measured = measure;
    // An NSZ is smaller than what it unpacks to, which is what installing it any other way takes.
    bool packed = false;
    for (auto& content : job.contents)
        packed = packed || (content.base && content.base->packed()) || (content.patch && content.patch->packed());
    if (packed) {
        estimate.packedBytes = job.inputBytes;
        estimate.installedBytes = CopyMark::installedBytes(job);
    }
    return estimate;
}

}  // namespace

SizeEstimate estimateTitle(const TitleJob& job, const ConvertOptions& options) { return workOut(job, options, false); }

SizeEstimate measureTitle(const TitleJob& job, const ConvertOptions& options) { return workOut(job, options, true); }

bool isAlreadyCompressed(const TitleJob& job, const ConvertOptions& options) {
    if (!options.compress || job.update)
        return false;
    for (auto& content : job.contents) {
        for (int i : content.sectionsInFileOrder()) {
            SectionSource source = {};
            content.sectionAt(i, source);
            if (TitleConverter::plan(content, source, options) != TitleConverter::Plan::CompressRomFs)
                continue;
            if (!source.section->hasCompression())
                return false;
        }
    }
    // A copy an older version made can come out smaller made again, which is for the console's own games to say. Which version made it is
    // in a game's control data, which DLC doesn't have, so DLC this tool made is taken as it is, whichever version made it.
    bool hasControl = false;
    for (auto& content : job.contents)
        hasControl = hasControl || content.type == Cnmt::ContentTypeControl;
    if (options.improveOlderCopies && hasControl && isMadeByThisTool(job)) {
        CopyMark mark;
        if (!CopyMark::in(job, mark) || !mark.isCurrent())
            return false;
    }
    // So can one its developer compressed, made again here, which is worked out like any other game. Its updates then need its original,
    // which is for whoever ticks it to know.
    if (options.improveOlderCopies && hasControl && !isMadeByThisTool(job) && hasCompressedGameData(job))
        return false;
    // Nothing compressing would touch, such as a game that keeps all its data in its program, is as small as it gets too. Otherwise its
    // compressed copy would come back on the list each time it's analyzed.
    return !job.contents.empty();
}

bool isMadeByThisTool(const TitleJob& job) {
    for (auto& content : job.contents) {
        NcaFile& nca = content.primary();
        for (int i = 0; i < NcaFile::SectionCount; i++) {
            const NcaFsSection* section = nca.section(i);
            if (section && section->hasCompression() && nca.isRebuiltCompressed(*section))
                return true;
        }
    }
    return false;
}

bool hasCompressedGameData(const TitleJob& job) {
    for (auto& content : job.contents) {
        NcaFile& nca = content.primary();
        for (int i = 0; i < NcaFile::SectionCount; i++) {
            const NcaFsSection* section = nca.section(i);
            if (section && section->hasCompression())
                return true;
        }
    }
    return false;
}

i64 unpackedBytes(const TitleJob& job, const ConvertOptions& options) {
    ConvertOptions plain = options;
    plain.compress = false;
    Tally tally;
    i64 total = 0;
    for (auto& content : job.contents) {
        i64 nca = NcaWriter::HeaderRegionSize;
        for (int i : content.sectionsInFileOrder()) {
            SectionSource source = {};
            content.sectionAt(i, source);
            nca += NcaWriter::alignToSector(sectionSize(content, source, plain, tally, false));
        }
        total += nca;
    }
    return total;
}

i64 freedBy(const TitleJob& job, const ConvertOptions& options) {
    CopyMark mark;
    i64 original = CopyMark::in(job, mark) ? mark.originalBytes : unpackedBytes(job, options);
    return std::max<i64>(0, original - job.inputBytes);
}

SizeEstimate alreadyCompressedEstimate(const TitleJob& job, const ConvertOptions& options) {
    SizeEstimate same;
    same.inputBytes = same.outputBytes = job.inputBytes;
    same.alreadyCompressed = true;
    bool ours = isMadeByThisTool(job);
    if (ours)
        same.freedBytes = freedBy(job, options);
    same.byMakers = !ours && hasCompressedGameData(job);
    return same;
}

bool CopyMark::isCurrent() const { return compareVersions(madeBy, StoresRepeatsOnce) >= 0; }

Bytes CopyMark::toBytes() const {
    std::string text = format("hacBrewPackLZ copy 1\nmade by %s\noriginal %lld\n", madeBy.c_str(), (long long)originalBytes);
    return Bytes(text.begin(), text.end());
}

bool CopyMark::parse(const Bytes& data, CopyMark& out) {
    std::string text(data.begin(), data.end());
    std::vector<std::string> lines;
    for (size_t at = 0; at <= text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        lines.push_back(text.substr(at, end - at));
        at = end + 1;
    }
    if (lines.size() < 3 || lines[0] != "hacBrewPackLZ copy 1" || lines[1].rfind("made by ", 0) != 0 || lines[2].rfind("original ", 0) != 0)
        return false;
    std::string digits = lines[2].substr(9);
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos || digits.size() > 18)
        return false;
    out.madeBy = lines[1].substr(8);
    out.originalBytes = std::stoll(digits);
    return true;
}

bool CopyMark::in(const TitleJob& job, CopyMark& out) {
    for (auto& content : job.contents) {
        if (content.type != Cnmt::ContentTypeControl)
            continue;
        NcaFile& nca = content.primary();
        for (int i = 0; i < NcaFile::SectionCount; i++) {
            const NcaFsSection* section = nca.section(i);
            if (!section || section->fsType() != NcaFsSection::FsRomFs || section->hasIndirect())
                continue;
            SourcePtr image = nca.openRomFsImage(*section, nullptr, false);
            RomFsArchive archive = RomFsArchive::open(image, format("%s section %d", nca.name().c_str(), i));
            for (auto& file : archive.files) {
                if (file.path != FileName || file.size > 4096)
                    continue;
                Bytes data((size_t)file.size);
                image->read(file.offset, data.data(), data.size());
                return parse(data, out);
            }
        }
    }
    return false;
}

CopyMark CopyMark::forJob(const TitleJob& job, const ConvertOptions& options) {
    CopyMark mark;
    mark.madeBy = EngineVersion;
    // For the tests, a copy as an older version would have made it, which then comes up to be made again.
    if (const char* test = getenv("HBP_TEST_MADE_BY"); test && *test)
        mark.madeBy = test;
    CopyMark before;
    if (in(job, before))
        mark.originalBytes = before.originalBytes;
    else if (isMadeByThisTool(job))
        mark.originalBytes = unpackedBytes(job, options);
    else
        mark.originalBytes = installedBytes(job);
    return mark;
}

i64 CopyMark::installedBytes(const TitleJob& job) {
    i64 total = 0;
    for (const ItemPtr& item : {job.main, job.update}) {
        if (!item)
            continue;
        for (auto& record : item->meta.records) {
            if (record.type != Cnmt::ContentTypeMeta && record.type != Cnmt::ContentTypeDeltaFragment)
                total += record.size;
        }
    }
    return total;
}

int CopyMark::compareVersions(const std::string& a, const std::string& b) {
    auto parts = [](const std::string& version) {
        std::vector<int> numbers;
        std::string plain = version.substr(0, version.find('-'));
        for (size_t at = 0; at <= plain.size();) {
            size_t end = plain.find('.', at);
            if (end == std::string::npos)
                end = plain.size();
            std::string part = plain.substr(at, end - at);
            numbers.push_back(!part.empty() && part.find_first_not_of("0123456789") == std::string::npos && part.size() < 9 ? std::stoi(part) : 0);
            at = end + 1;
        }
        return numbers;
    };
    std::vector<int> x = parts(a), y = parts(b);
    for (size_t i = 0; i < std::max(x.size(), y.size()); i++) {
        int p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q)
            return p < q ? -1 : 1;
    }
    return 0;
}

bool updateFits(const TitleJob& job) {
    constexpr int Samples = 32;
    try {
        for (auto& content : job.contents) {
            if (!content.base || !content.patch)
                continue;
            for (int i = 0; i < NcaFile::SectionCount; i++) {
                const NcaFsSection* section = content.patch->section(i);
                if (!section || !section->hasIndirect() || section->fsType() != NcaFsSection::FsRomFs || !content.base->section(i))
                    continue;
                SourcePtr image = content.patch->openRomFsImage(*section, content.base.get(), true);
                Bytes buffer((size_t)IvfcBuilder::BlockSize);
                for (int sample = 0; sample < Samples; sample++) {
                    i64 at = image->length() * sample / Samples / IvfcBuilder::BlockSize * IvfcBuilder::BlockSize;
                    image->read(at, buffer.data(), (size_t)std::min<i64>((i64)buffer.size(), image->length() - at));
                }
                image->release();
            }
        }
        return true;
    } catch (const MismatchError&) {
        // Anything else, such as a card that won't be read or a key that's missing, is said as it is, rather than as an update that doesn't fit.
        return false;
    }
}

void checkTitle(const TitleJob& job, const std::string& tempDir) {
    NczSource::setSpoolFolder(tempDir);
    struct Release {
        const TitleJob& job;
        ~Release() { job.releaseMemory(); }
    } release{job};

    // Opened first, to know how much there is to read.
    std::vector<std::unique_ptr<Pfs0Archive>> archives;
    std::vector<SourcePtr> parts;
    i64 total = 0;
    for (auto& content : job.contents) {
        for (int i : content.sectionsInFileOrder()) {
            SectionSource source = {};
            content.sectionAt(i, source);
            if (source.section->fsType() == NcaFsSection::FsPartition) {
                archives.push_back(source.owner->openPfs0(*source.section, source.original));
                parts.push_back(archives.back()->data());
            } else {
                parts.push_back(source.owner->openRomFsImage(*source.section, source.original));
            }
            total += parts.back()->length();
        }
    }

    Progress progress("正在读取全部内容", total);
    Bytes buffer(1 << 20);
    i64 done = 0;
    for (auto& part : parts) {
        i64 length = part->length();
        for (i64 position = 0; position < length;) {
            size_t n = (size_t)std::min<i64>((i64)buffer.size(), length - position);
            part->read(position, buffer.data(), n);
            position += (i64)n;
            done += (i64)n;
            progress.report(done);
        }
        part->release();
    }
    progress.done();
}

SizeEstimate estimateBuild(const ExtractedGame& game, const ConvertOptions& given, bool measure) {
    ConvertOptions options = given;
    if (options.threads <= 0)
        options.threads = sys::workerCount();

    Tally tally;
    i64 program = NcaWriter::HeaderRegionSize;
    Bytes npdm = readWholeFile(game.folder + "/exefs/main.npdm");
    program += NcaWriter::alignToSector(Sections::pfs0SectionSize(buildPfs0FromFolder(game.folder + "/exefs", {{"main.npdm", npdm}})->length(), Sections::ExefsHashBlockSize));
    if (game.hasLogo)
        program += NcaWriter::alignToSector(Sections::pfs0SectionSize(buildPfs0FromFolder(game.folder + "/logo")->length(), Sections::LogoHashBlockSize));
    if (game.hasRomFs) {
        SourcePtr image = game.romFsImage;
        program += NcaWriter::alignToSector(!options.compress ? plainRomFsSize(image->length(), tally)
                                            : measure       ? measuredRomFsSize(image, options, tally, nullptr)
                                                            : compressedRomFsSize(image, options, tally, false));
    }

    auto romFsNcaSize = [](i64 imageSize) { return NcaWriter::HeaderRegionSize + NcaWriter::alignToSector(Sections::romFsSectionSize(imageSize)); };
    i64 output = program + romFsNcaSize(buildRomFsImage(game.folder + "/control", {{"control.nacp", patchedNacp(game)}})->length());
    if (game.hasHtmlDoc)
        output += romFsNcaSize(buildRomFsImage(game.folder + "/htmldoc")->length());
    if (game.hasLegalInfo)
        output += romFsNcaSize(buildRomFsImage(game.folder + "/legalinfo")->length());
    output += NspOutput::headerSize(game.contentCount()) + metaNcaSize(Cnmt::fileName(game.titleId, Cnmt::MetaApplication), builtCnmtSize(game.contentCount()));

    SizeEstimate estimate;
    estimate.inputBytes = game.inputBytes;
    estimate.outputBytes = output;
    estimate.marginBytes = (i64)ceil(sqrt(tally.variance));
    estimate.romFsBytes = tally.romFs;
    estimate.romFsStoredBytes = tally.stored;
    estimate.measured = measure;
    return estimate;
}

namespace SavingRule {

bool applies(const ConvertOptions& options) { return options.compress && options.minSavingPercent > 0; }

/// The minimum goes by what it's sure to save.
bool worthCompressing(const SizeEstimate& estimate, const ConvertOptions& options) { return !applies(options) || estimate.sureSavedFraction() * 100 >= options.minSavingPercent; }

bool notSure(const SizeEstimate& estimate, const ConvertOptions& options) {
    // Sure to save less than this is nothing, whatever the minimum: it's within the margin of error. As the app and the PC have it.
    constexpr double NothingToGainShare = 0.005;
    // What compressing can't shrink is stored as it is, so a first look putting a game over its size is no fact about it: such games,
    // compressed, came to a few kilobytes less, and may save more. Only growing that is a fact leaves a game out on its say (sureToGrow): an
    // NSZ made into a file, compared with that file, which is packed tighter than LZ4 packs. As SavingRule.NotSure on the PC.
    if (!options.compress || estimate.measured || estimate.alreadyCompressed || estimate.sureToGrow(options.compress))
        return false;
    return estimate.savedBytes() < 0 || estimate.sureSavedFraction() < NothingToGainShare || !worthCompressing(estimate, options);
}

std::string reason(const SizeEstimate& estimate, const ConvertOptions& options) {
    if (estimate.savedBytes() < 0)
        return applies(options) ? format("会增加 %.0f%%，低于 %d%% 的下限", -estimate.savedFraction() * 100, options.minSavingPercent)
                                : format("会增加 %.0f%%", -estimate.savedFraction() * 100);
    double percent = estimate.sureSavedFraction() * 100;
    // Rounded, just under the minimum would read as the minimum itself.
    bool close = percent >= options.minSavingPercent - 0.5;
    std::string figure = format(close ? "%.1f%%" : "%.0f%%", close ? floor(percent * 10) / 10 : percent);
    return format(estimate.atLeast() && !estimate.measured ? "仅能确定节省 %s，低于 %d%% 的下限" : "仅节省 %s，低于 %d%% 的下限", figure.c_str(),
                  options.minSavingPercent);
}

}  // namespace SavingRule

}  // namespace hbp
