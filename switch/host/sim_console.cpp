#include "sim_console.h"

#include <algorithm>
#include <map>
#include <set>

namespace hbp {

namespace {

constexpr i64 SimTotal = 64LL << 30;

Bytes readAll(const std::string& path) {
    sys::File file(path, sys::File::Mode::Read);
    Bytes data((size_t)file.size());
    if (!data.empty())
        file.read(0, data.data(), data.size());
    return data;
}

void writeAll(const std::string& path, const void* data, size_t size) {
    // All at once, as a commit to the real database is.
    std::string temp = path + ".new";
    {
        sys::File file(temp, sys::File::Mode::Create);
        file.write(0, data, size);
    }
    sys::removeFile(path);
    sys::renameFile(temp, path);
}

std::string keyName(const MetaKey& key) { return format("%016llx_%u_%d", (unsigned long long)key.id, key.version, key.type); }

class SimWriter : public ContentWriter {
public:
    SimWriter(SimConsole& console, Storage storage, std::string token, i64 size) : _console(console), _storage(storage), _token(std::move(token)), _size(size), _file(new sys::File(console.placeholderPath(storage, _token), sys::File::Mode::Create)) {}

    ~SimWriter() override {
        if (_file) {
            _file.reset();
            if (!_console.powerLost())
                sys::removeFile(_console.placeholderPath(_storage, _token));
        }
    }

    std::string token() const override { return _token; }

    void write(i64 offset, const void* data, size_t size) override {
        _console.tick();
        i64 end = offset + (i64)size;
        i64 free = _console.freeFor(_storage);
        if (free >= 0 && end > free)
            throw ToolError("There isn't enough room to write the new copy");
        if (_size >= 0 && end > _size)
            throw ToolError("内部错误：实际写入量大于占位预留量");
        _file->write(offset, data, size);
        _end = std::max(_end, end);
    }

    void setSize(i64 size) override {
        _console.tick();
        _file->setSize(size);
        _size = size;
    }

    void holdAs(const ContentId& temporary) override {
        _console.tick();
        _file.reset();
        sys::renameFile(_console.placeholderPath(_storage, _token), _console.contentPath(_storage, temporary));
        _held = temporary;
        _isHeld = true;
    }

    void finishAs(const ContentId& id) override {
        if (_isHeld) {
            // Back to being a placeholder, and then on to its real name, as two steps that the power can go between.
            _console.tick();
            sys::renameFile(_console.contentPath(_storage, _held), _console.placeholderPath(_storage, _token));
        } else if (_size >= 0 && _end != _size) {
            throw ToolError("内部错误：实际写入量小于占位预留量");
        }
        _console.tick();
        _file.reset();
        sys::renameFile(_console.placeholderPath(_storage, _token), _console.contentPath(_storage, id));
    }

private:
    SimConsole& _console;
    Storage _storage;
    std::string _token;
    i64 _size;
    std::unique_ptr<sys::File> _file;
    i64 _end = 0;
    ContentId _held;
    bool _isHeld = false;
};

class SimContent : public DataSource {
public:
    SimContent(SimConsole& console, const std::string& path, i64 size) : _console(console), _path(path), _size(size) {}
    i64 length() const override { return _size; }
    void read(i64 offset, void* buffer, size_t count) override {
        _console.tick();
        // The real thing holds nothing open between reads, so what's being read can be deleted.
        sys::File file(_path, sys::File::Mode::Read);
        file.read(offset, buffer, count);
        if (_console.cardCorrupts() && count > 0)
            ((u8*)buffer)[count / 2] ^= 0x40;
    }

private:
    SimConsole& _console;
    std::string _path;
    i64 _size;
};

}  // namespace

SimConsole::SimConsole(std::string folder, const Keyset& keyset) : _folder(std::move(folder)), _keyset(keyset) {
    for (Storage storage : {Storage::Sd, Storage::Nand}) {
        sys::makeDirectories(storageFolder(storage) + "/contents");
        sys::makeDirectories(storageFolder(storage) + "/placeholders");
        sys::makeDirectories(storageFolder(storage) + "/metas");
    }
    sys::makeDirectories(_folder + "/records");
    sys::makeDirectories(_folder + "/tickets");
    sys::makeDirectories(_folder + "/hashes");

    // Placeholders from before a power cut stay where they are, but new ones mustn't collide with them.
    for (Storage storage : {Storage::Sd, Storage::Nand}) {
        for (auto& entry : sys::list(storageFolder(storage) + "/placeholders"))
            _nextPlaceholder = std::max(_nextPlaceholder, atoi(entry.name.c_str()) + 1);
    }
}

void SimConsole::tick() {
    _calls++;
    if (_powerCutAt > 0 && _calls >= _powerCutAt) {
        _powerLost = true;
        throw PowerCut();
    }
}

std::string SimConsole::storageFolder(Storage storage) const { return _folder + (storage == Storage::Sd ? "/sd" : "/nand"); }
std::string SimConsole::contentPath(Storage storage, const ContentId& id) const { return storageFolder(storage) + "/contents/" + id.hex() + ".nca"; }
std::string SimConsole::placeholderPath(Storage storage, const std::string& token) const { return storageFolder(storage) + "/placeholders/" + token; }
std::string SimConsole::metaPath(Storage storage, const MetaKey& key) const { return storageFolder(storage) + "/metas/" + keyName(key); }
std::string SimConsole::recordsPath(u64 applicationId) const { return _folder + format("/records/%016llx.txt", (unsigned long long)applicationId); }

void SimConsole::install(const std::vector<std::string>& nspPaths, Storage storage) {
    std::map<u64, std::vector<std::pair<MetaKey, Storage>>> byApplication;
    for (auto& path : nspPaths) {
        auto archive = std::make_shared<FileArchive>(path);
        for (auto& entry : archive->entries()) {
            if (endsWithNoCase(entry.name, ".tik")) {
                Bytes ticket((size_t)entry.size);
                archive->open(entry)->read(0, ticket.data(), ticket.size());
                std::string name = _folder + "/tickets/" + entry.name;
                writeAll(name, ticket.data(), ticket.size());
                continue;
            }
            if (!endsWithNoCase(entry.name, ".nca"))
                continue;
            ContentId id;
            if (!ContentId::parse(entry.name.substr(0, 32), id))
                throw ToolError("Unexpected NCA name in " + path + ": " + entry.name);
            SourcePtr source = archive->open(entry);
            sys::File out(contentPath(storage, id), sys::File::Mode::Create);
            Bytes buffer(1 << 20);
            Sha256 hash;
            for (i64 done = 0; done < entry.size;) {
                size_t n = (size_t)std::min<i64>((i64)buffer.size(), entry.size - done);
                source->read(done, buffer.data(), n);
                out.write(done, buffer.data(), n);
                hash.update(buffer.data(), n);
                done += (i64)n;
            }
            // Not every game file's NCAs are named after their hash, and that needn't mean anything's wrong with them.
            u8 result[32];
            hash.finish(result);
            if (memcmp(result, id.bytes, 16) != 0)
                writeAll(_folder + "/hashes/" + id.hex(), result, 16);
        }

        TitleSet set(_keyset);
        set.add(archive);
        TicketStore tickets(_keyset);
        for (auto& entry : archive->entries()) {
            if (!endsWithNoCase(entry.name, ".cnmt.nca"))
                continue;
            NcaFile nca(archive->open(entry), entry.name, _keyset, tickets);
            const NcaFsSection* section = nullptr;
            for (int i = 0; i < NcaFile::SectionCount && !section; i++) {
                if (nca.section(i) && nca.section(i)->fsType() == NcaFsSection::FsPartition)
                    section = nca.section(i);
            }
            auto pfs = nca.openPfs0(*section, nullptr);
            for (auto& file : pfs->files()) {
                if (!endsWithNoCase(file.name, ".cnmt"))
                    continue;
                ContentMeta meta = ContentMeta::parse(pfs->readAll(file), entry.name);
                NcaInfo info;
                ContentId metaId;
                ContentId::parse(entry.name.substr(0, 32), metaId);
                memcpy(info.hash, metaId.bytes, 16);
                info.size = entry.size;
                MetaKey key{meta.titleId, meta.version, meta.type, 0};
                Bytes blob = installMetaFrom(meta, info);
                writeAll(metaPath(storage, key), blob.data(), blob.size());

                u64 application = meta.type == Cnmt::MetaApplication ? meta.titleId : meta.linkedId();
                if (meta.type == Cnmt::MetaAddOnContent)
                    application = meta.linkedId();
                byApplication[application].emplace_back(key, storage);
            }
        }
    }
    for (auto& item : byApplication) {
        auto existing = records(item.first);
        for (auto& record : item.second) {
            if (std::find_if(existing.begin(), existing.end(), [&](auto& e) { return e.first == record.first; }) == existing.end())
                existing.push_back(record);
        }
        setRecords(item.first, existing);
    }
}

std::vector<u64> SimConsole::listApplications() {
    tick();
    std::vector<u64> list;
    for (auto& entry : sys::list(_folder + "/records"))
        list.push_back(strtoull(entry.name.c_str(), nullptr, 16));
    std::sort(list.begin(), list.end());
    return list;
}

InstalledGame SimConsole::describe(u64 applicationId) {
    tick();
    InstalledGame game;
    game.applicationId = applicationId;
    for (auto& record : records(applicationId)) {
        Bytes blob = readAll(metaPath(record.second, record.first));
        InstalledMeta meta;
        meta.key = record.first;
        meta.storage = record.second;
        int extended = rd16(blob.data());
        int count = rd16(blob.data() + 2);
        for (int i = 0; i < count; i++) {
            const u8* info = blob.data() + 8 + extended + i * 0x18;
            InstalledContent content;
            memcpy(content.id.bytes, info, 16);
            content.size = (i64)rd32(info + 0x10) | (i64)info[0x14] << 32;
            content.type = info[0x16];
            content.idOffset = info[0x17];
            meta.contents.push_back(content);
        }
        game.metas.push_back(meta);
    }
    return game;
}

void SimConsole::readControl(InstalledGame& game) {
    tick();
    auto appearance = _appearance.find(game.applicationId);
    game.name = appearance != _appearance.end() ? appearance->second.first : format("Game %016llx", (unsigned long long)game.applicationId);
    if (appearance != _appearance.end())
        game.icon = appearance->second.second;
    controlReads++;
}

SourcePtr SimConsole::openContent(Storage storage, const ContentId& id, i64 size) {
    tick();
    std::string path = contentPath(storage, id);
    if (!sys::exists(path))
        throw ToolError("Content " + id.hex() + " isn't there");
    return std::make_shared<SimContent>(*this, path, size);
}

void SimConsole::addTitleKeys(TicketStore& store, const Keyset&) {
    tick();
    for (auto& entry : sys::list(_folder + "/tickets")) {
        Bytes ticket = readAll(_folder + "/tickets/" + entry.name);
        store.addTicket(ticket.data(), ticket.size());
    }
}

i64 SimConsole::freeBytes(Storage storage) {
    tick();
    i64 set = _free[storage == Storage::Sd ? 1 : 0];
    return set >= 0 ? set : SimTotal;
}

i64 SimConsole::totalBytes(Storage) { return SimTotal; }

std::unique_ptr<ContentWriter> SimConsole::createContent(Storage storage, i64 size) {
    tick();
    return std::make_unique<SimWriter>(*this, storage, format("%d", _nextPlaceholder++), size);
}

void SimConsole::deletePlaceholder(Storage storage, const std::string& token) {
    tick();
    sys::removeFile(placeholderPath(storage, token));
}

bool SimConsole::hasContent(Storage storage, const ContentId& id) {
    tick();
    return sys::exists(contentPath(storage, id));
}

void SimConsole::deleteContent(Storage storage, const ContentId& id) {
    tick();
    sys::removeFile(contentPath(storage, id));
}

void SimConsole::setMeta(Storage storage, const MetaKey& key, const Bytes& installMeta) {
    tick();
    writeAll(metaPath(storage, key), installMeta.data(), installMeta.size());
}

bool SimConsole::hasMeta(Storage storage, const MetaKey& key) {
    tick();
    return sys::exists(metaPath(storage, key));
}

void SimConsole::removeMeta(Storage storage, const MetaKey& key) {
    tick();
    sys::removeFile(metaPath(storage, key));
}

void SimConsole::setRecords(u64 applicationId, const std::vector<std::pair<MetaKey, Storage>>& list) {
    tick();
    std::string text;
    for (auto& record : list)
        text += format("%016llx %u %d %d\n", (unsigned long long)record.first.id, record.first.version, record.first.type, (int)record.second);
    if (list.empty())
        sys::removeFile(recordsPath(applicationId));
    else
        writeAll(recordsPath(applicationId), text.data(), text.size());
}

std::vector<std::pair<MetaKey, Storage>> SimConsole::records(u64 applicationId) {
    std::vector<std::pair<MetaKey, Storage>> list;
    std::string path = recordsPath(applicationId);
    if (!sys::exists(path))
        return list;
    Bytes data = readAll(path);
    std::string text(data.begin(), data.end());
    size_t position = 0;
    while (position < text.size()) {
        size_t end = text.find('\n', position);
        unsigned long long id;
        unsigned version;
        int type, storage;
        if (sscanf(text.substr(position, end - position).c_str(), "%llx %u %d %d", &id, &version, &type, &storage) == 4) {
            MetaKey key;
            key.id = id;
            key.version = version;
            key.type = (u8)type;
            list.emplace_back(key, (Storage)storage);
        }
        position = end + 1;
    }
    return list;
}

std::unique_ptr<FirmwareView> SimConsole::mountGameData(Storage storage, const ContentId& id, u64) {
    tick();
    if (_firmwareRefuses)
        throw ToolError("(2002-4502) NCA signature check failed");
    std::string path = contentPath(storage, id);
    i64 size;
    {
        sys::File file(path, sys::File::Mode::Read);
        size = file.size();
    }
    SourcePtr raw = std::make_shared<SimContent>(*this, path, size);
    // As the real thing does: only a Data NCA can be mounted this way.
    TicketStore tickets(_keyset);
    NcaFile nca(raw, id.hex(), _keyset, tickets);
    if (nca.contentType() != NcaContentType::Data && nca.contentType() != NcaContentType::PublicData)
        throw ToolError("(2002-4400) That's not a Data NCA");
    return readGameDataOf(raw, id.hex(), _keyset);
}

std::unique_ptr<FirmwareView> SimConsole::mountLogo(Storage storage, const ContentId& id, u64) {
    tick();
    if (_firmwareRefuses)
        throw ToolError("(2002-4502) NCA signature check failed");
    std::string path = contentPath(storage, id);
    i64 size;
    {
        sys::File file(path, sys::File::Mode::Read);
        size = file.size();
    }
    return readLogoOf(std::make_shared<SimContent>(*this, path, size), id.hex(), _keyset);
}

std::string SimConsole::checkConsistent(const Keyset&) {
    int saved = _powerCutAt;
    _powerCutAt = 0;
    std::string problem;
    for (u64 application : listApplications()) {
        InstalledGame game = describe(application);
        if (game.metas.empty())
            problem = format("%016llx has no content left", (unsigned long long)application);
        bool hasBase = false;
        for (auto& meta : game.metas) {
            hasBase = hasBase || meta.key.type == Cnmt::MetaApplication;
            for (auto& content : meta.contents) {
                std::string path = contentPath(meta.storage, content.id);
                if (!sys::exists(path)) {
                    problem = format("%016llx v%u is missing %s", (unsigned long long)meta.key.id, meta.key.version, content.id.hex().c_str());
                    continue;
                }
                // Each file's name is the start of its hash, and the whole of it has to be there and right.
                Bytes data = readAll(path);
                u8 hash[32];
                sha256(data.data(), data.size(), hash);
                Bytes expected(content.id.bytes, content.id.bytes + 16);
                std::string otherHash = _folder + "/hashes/" + content.id.hex();
                if (sys::exists(otherHash))
                    expected = readAll(otherHash);
                if ((i64)data.size() != content.size || memcmp(hash, expected.data(), 16) != 0)
                    problem = format("%016llx v%u has a damaged %s", (unsigned long long)meta.key.id, meta.key.version, content.id.hex().c_str());
            }
        }
        if (!hasBase)
            problem = format("%016llx has no base game", (unsigned long long)application);
    }
    _powerCutAt = saved;
    return problem;
}

std::vector<std::string> SimConsole::strayFiles() {
    std::set<std::string> used;
    int saved = _powerCutAt;
    _powerCutAt = 0;
    for (u64 application : listApplications()) {
        for (auto& meta : describe(application).metas) {
            for (auto& content : meta.contents)
                used.insert(content.id.hex());
        }
    }
    _powerCutAt = saved;

    std::vector<std::string> stray;
    for (Storage storage : {Storage::Sd, Storage::Nand}) {
        for (auto& entry : sys::list(storageFolder(storage) + "/contents")) {
            if (!used.count(entry.name.substr(0, 32)))
                stray.push_back("content " + entry.name);
        }
        for (auto& entry : sys::list(storageFolder(storage) + "/placeholders"))
            stray.push_back("placeholder " + entry.name);
    }
    return stray;
}

}  // namespace hbp
