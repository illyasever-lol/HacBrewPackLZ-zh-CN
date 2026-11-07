#include "title.h"

#include <deque>
#include <algorithm>
#include <cctype>
#include <cmath>
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
        throw ToolError(what + "的内容元数据已损坏");
    const u8* r = bytes.data();
    int extended = rd16(r + 0x0E);
    int count = rd16(r + 0x10);
    if (HeaderSize + extended + count * (i64)RecordSize > (i64)bytes.size())
        throw ToolError(what + "的内容元数据已损坏");

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
    if (count < 0 || count > MaxEntries || nameTable < 0 || 0x10 + tableSize > _length)
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
        if (offset < 0 || size < 0 || dataStart + offset + size > _length || nameOffset < 0 || nameOffset >= nameTable)
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
    if (count < 0 || count > MaxEntries || nameTable < 0 || start + Hfs0HeaderSize + tableSize > _length)
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
        if (mustFit && dataStart + offset + size > _length)
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
                throw ToolError(entry.name + "没有 PFS0 区段");
            auto pfs = nca->openPfs0(*section, nullptr);
            const Pfs0Archive::Entry* cnmt = nullptr;
            for (auto& file : pfs->files()) {
                if (endsWithNoCase(file.name, ".cnmt"))
                    cnmt = &file;
            }
            if (!cnmt)
                throw ToolError(entry.name + "中没有 .cnmt 文件");
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
                if (record.type == Cnmt::ContentTypeProgram && item->meta.type == Cnmt::MetaApplication)
                    summary.holdsOnlyPart = summary.holdsOnlyPart || openContent(*item, record)->holdsOnlyPart();
                else if (record.type == Cnmt::ContentTypeControl)
                    readNameAndIcon(*openContent(*item, record), summary);
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
                    throw ToolError("这只包含游戏的一部分，其余内容在升级补丁里，两者缺一不可");
            }
        }

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
        throw ToolError("'" + fileNameOf(item.archive->path()) + "' 缺少" + name + "，而该内容在其内容元数据中有列出");
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

i64 NspOutput::headerSize(int contentCount) {
    int nameTable = (contentCount * (NcaNameLength + 1) + MetaNcaNameLength + 1 + 0x1F) & ~0x1F;
    return 0x10 + (contentCount + 1) * PfsEntrySize + nameTable;
}

NspOutput::NspOutput(std::string path, int contentCount, i64 sizeHint)
    : _path(std::move(path)), _partPath(_path + ".part"), _file(_partPath, sys::File::Mode::Create, sizeHint), _dataStart(headerSize(contentCount)), _expectedFiles(contentCount + 1), _position(_dataStart) {
    Bytes reserved((size_t)_dataStart);
    _file.write(0, reserved.data(), reserved.size());
    _reserved = sizeHint > 0 && _file.reserve(sizeHint);
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
    _file.write(_position, data, size);
    _position += (i64)size;
}

void NspOutput::overwrite(i64 offset, const void* data, size_t size) { _file.write(_ncaStart + offset, data, size); }

void NspOutput::readBack(i64 offset, void* data, size_t size) { _file.read(_ncaStart + offset, data, size); }

void NspOutput::endNca(const NcaInfo& info, const char* extension) { _files.push_back(FileRecord{info.id() + extension, _ncaStart - _dataStart, info.size}); }

void NspOutput::finish(const ContentMeta&, const NcaInfo&) {
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
    if (_reserved)
        _file.setSize(_position);
    _file.flush();
    _file.close();
    sys::removeFile(_path);
    sys::renameFile(_partPath, _path);
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

static constexpr u32 CompressionMinSystemVersion = 12u << 26;

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
    for (auto& content : job.contents)
        records.push_back(ContentRecord{writeContent(output, content, compressed), content.type, content.idOffset});

    const ContentMeta& meta = job.main->meta;
    u32 required = std::max(meta.requiredSystemVersion(), job.update ? job.update->meta.requiredSystemVersion() : 0u);
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
        if (plan(content, source, _options) == Plan::CompressRomFs)
            compressInto(builder, i, source.owner->openRomFsImage(*source.section, source.original), "正在压缩游戏数据");
        else
            builder.add(i, buildSection(content, source, sign));
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
        throw ToolError(what + "不是有效的 NPDM（META 魔数错误）");
    int aci0 = rdi32(&npdm[0x70]);
    int acid = rdi32(&npdm[0x78]);
    if (aci0 < 0 || (size_t)aci0 + 0x18 > npdm.size() || memcmp(&npdm[(size_t)aci0], "ACI0", 4) != 0)
        throw ToolError(what + "不是有效的 NPDM（ACI0 区段错误）");
    if (acid < 0 || (size_t)acid + 0x204 > npdm.size() || memcmp(&npdm[(size_t)acid + 0x200], "ACID", 4) != 0)
        throw ToolError(what + "不是有效的 NPDM（ACID 区段错误）");
    memcpy(&npdm[(size_t)acid + 0x100], signingModulus(), 0x100);
    return npdm;
}

NcaSection TitleConverter::buildSection(const ContentPair& content, const SectionSource& source, bool& sign) {
    NcaFile& owner = *source.owner;
    const NcaFsSection& section = *source.section;
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

void TitleConverter::compressInto(NcaBuilder& builder, int index, const SourcePtr& image, const char* label) {
    Progress progress(label, image->length());
    CompressedData data;
    builder.addCompressed(index, image, _options.blockSize, _options.level, _options.threads, true, progress, &data);
    progress.done();
    sink().message(format("Game data of %s stored as %s (%.1f%%)", formatSize(data.virtualSize).c_str(), formatSize(data.dataSize).c_str(), data.dataSize * 100.0 / (double)std::max<i64>(data.virtualSize, 1)));
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
        throw ToolError("'" + relative + "' 会覆盖该文件系统上的另一文件（其文件名不区分大小写或无法容纳所有字符）");
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
                for (auto& file : archive->files()) {
                    copyOut(*archive->data(), file.offset, file.size, targetPath(folder, file.name, seen), buffer.get(), CopyBufferSize, nullptr, 0);
                    result.bytes += file.size;
                    result.files++;
                }
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

SampleResult summarize(const std::vector<RegionResult>& samples, i64 imageSize, i64 regions) {
    double sampled = 0, storedTotal = 0;
    i64 lz4 = 0, raw = 0;
    for (auto& s : samples) {
        sampled += s.length;
        storedTotal += s.stored;
        lz4 += s.lz4Blocks;
        raw += s.rawBlocks;
    }
    if (sampled == 0)
        throw ToolError("无法读取任何游戏数据用于采样");
    double ratio = storedTotal / sampled;
    double scale = (double)imageSize / sampled;

    // Neighbouring regions tend to hold similar data, so differences between neighbours give the honest spread for an evenly
    // spread sample. An image small enough to read completely has none.
    std::vector<RegionResult> ordered = samples;
    std::stable_sort(ordered.begin(), ordered.end(), [](auto& a, auto& b) { return a.region < b.region; });
    double squares = 0;
    for (size_t i = 1; i < ordered.size(); i++) {
        double difference = ordered[i].stored / (double)ordered[i].length - ordered[i - 1].stored / (double)ordered[i - 1].length;
        squares += difference * difference;
    }
    double count = (double)ordered.size();
    double unsampled = 1 - count / (double)regions;
    double variance = count > 1 ? unsampled / count * squares / (2.0 * (count - 1)) : 0;

    SampleResult result;
    result.stored = (i64)(ratio * (double)imageSize);
    result.margin = Confidence95 * sqrt(variance) * (double)imageSize;
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
SampleResult sample(const SourcePtr& image, const ConvertOptions& options, bool inOrder) {
    i64 length = image->length();
    i64 regions = (length + RegionSize - 1) / RegionSize;
    if (regions == 0)
        return SampleResult{};

    i64 stride = (i64)((double)regions * GoldenRatioConjugate);
    while (greatestCommonDivisor(stride, regions) != 1)
        stride++;

    // A small image takes up less of the answer, and doesn't need as close a look.
    int minimum = (int)std::min<i64>(MinRegions, std::max<i64>(regions / 16, SmallestSample));
    double budget = TimeBudgetSeconds * std::min(1.0, (double)length / (1LL << 30));

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

    auto compressRegion = [&](const u8* data, int regionLength, i64 region, int worker) {
        int stored = 0, lz4 = 0, raw = 0;
        for (int done = 0; done < regionLength; done += options.blockSize) {
            int n = std::min(options.blockSize, regionLength - done);
            int size = encoders[(size_t)worker]->compress(data + done, n, scratch[(size_t)worker].get(), RomFsCompressor::compressCapacity(n), options.level);
            if (size > 0) {
                stored += size + RomFsCompressor::AveragePadding;
                lz4++;
            } else {
                stored += n;
                raw++;
            }
        }
        return RegionResult{region, regionLength, stored, lz4, raw};
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
        result = summarize(samples, length, regions);
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

i64 compressedRomFsSize(const SourcePtr& image, const ConvertOptions& options, Tally& tally, bool inOrder) {
    SampleResult s = sample(image, options, inOrder);
    tally.romFs += image->length();
    tally.stored += s.stored;
    tally.variance += s.margin * s.margin;

    // Raw blocks in a row share one table entry, so there can't be more raw entries than gaps between compressed blocks.
    i64 entries = s.lz4Blocks + std::min(s.rawBlocks, s.lz4Blocks + 1);
    return Sections::compressedRomFsSectionSize(s.stored, (int)std::max<i64>(std::min<i64>(entries, INT32_MAX), 1));
}

i64 plainRomFsSize(i64 image, Tally& tally) {
    tally.romFs += image;
    tally.stored += image;
    return Sections::romFsSectionSize(image);
}

i64 sectionSize(const ContentPair& content, const SectionSource& source, const ConvertOptions& options, Tally& tally) {
    NcaFile& owner = *source.owner;
    const NcaFsSection& section = *source.section;
    switch (TitleConverter::plan(content, source, options)) {
        case TitleConverter::Plan::CompressRomFs: {
            bool inOrder = owner.readsInOrder() || (source.original && source.original->readsInOrder());
            return compressedRomFsSize(owner.openRomFsImage(section, source.original, false), options, tally, inOrder);
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

SizeEstimate estimateTitle(const TitleJob& job, const ConvertOptions& given) {
    ConvertOptions options = given;
    if (options.threads <= 0)
        options.threads = sys::workerCount();

    struct Release {
        const TitleJob& job;
        ~Release() { job.releaseMemory(); }
    } release{job};

    Tally tally;
    i64 output = NspOutput::headerSize((int)job.contents.size());
    for (auto& content : job.contents) {
        i64 nca = NcaWriter::HeaderRegionSize;
        for (int i : content.sectionsInFileOrder()) {
            SectionSource source = {};
            content.sectionAt(i, source);
            nca += NcaWriter::alignToSector(sectionSize(content, source, options, tally));
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
    return estimate;
}

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
    // Nothing compressing would touch, such as a game that keeps all its data in its program, is as small as it gets too. Otherwise its
    // compressed copy would come back on the list each time it's analyzed.
    return !job.contents.empty();
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

SizeEstimate estimateBuild(const ExtractedGame& game, const ConvertOptions& given) {
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
        program += NcaWriter::alignToSector(options.compress ? compressedRomFsSize(image, options, tally, false) : plainRomFsSize(image->length(), tally));
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
    return estimate;
}

namespace SavingRule {

bool applies(const ConvertOptions& options) { return options.compress && options.minSavingPercent > 0; }

bool worthCompressing(const SizeEstimate& estimate, const ConvertOptions& options) { return !applies(options) || estimate.savedFraction() * 100 >= options.minSavingPercent; }

std::string reason(const SizeEstimate& estimate, const ConvertOptions& options) {
    double percent = estimate.savedFraction() * 100;
    if (estimate.savedBytes() >= 0)
        return format("仅节省 %.0f%%，低于 %d%% 的最低要求", percent, options.minSavingPercent);
    return format("反而增大 %.0f%%，低于 %d%% 的最低要求", -percent, options.minSavingPercent);
}

}  // namespace SavingRule

}  // namespace hbp
