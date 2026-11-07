// The real console: what's installed and how to read it (ns, ncm), its tickets (es), and the firmware's own way of mounting a game (fs).
#include "nx_console.h"

#include <switch.h>

#include <algorithm>
#include <array>

#include "nx_ipc.h"

namespace hbp {

namespace {

std::string codeOf(Result rc) { return format("%04u-%04u", 2000 + R_MODULE(rc), R_DESCRIPTION(rc)); }

/// Throws what went wrong, in words, with the system's code to go with it.
void check(Result rc, const char* what) {
    if (R_FAILED(rc))
        throw ToolError(format("%s (%s)", what, codeOf(rc).c_str()));
}

NcmStorageId storageIdOf(Storage storage) {
    switch (storage) {
        case Storage::Sd:
            return NcmStorageId_SdCard;
        case Storage::Nand:
            return NcmStorageId_BuiltInUser;
        case Storage::GameCard:
            return NcmStorageId_GameCard;
        default:
            return NcmStorageId_None;
    }
}

NcmContentId toNcm(const ContentId& id) {
    NcmContentId out;
    memcpy(out.c, id.bytes, sizeof(out.c));
    return out;
}

NcmContentMetaKey toNcm(const MetaKey& key) {
    NcmContentMetaKey out = {};
    out.id = key.id;
    out.version = key.version;
    out.type = key.type;
    out.install_type = key.installType;
    return out;
}

/// What's installed, read straight from the file system. The content manager can do this too, but it goes through another process for every
/// read and only keeps one file open at a time, which is a lot of opening and closing for a game with an update, whose reads go back and
/// forth between two of them. Anything that doesn't work here falls back on the content manager.
class DirectReader {
public:
    ~DirectReader() {
        _open.clear();
        for (auto& storage : _storages) {
            if (storage.mounted)
                fsFsClose(&storage.fs);
        }
    }

    /// Nothing is holding a file open by the time this returns, unless it's being read this very moment, so it can be changed or deleted.
    void closeAll() {
        sys::Lock lock(_mutex);
        _open.clear();
    }

    void read(NcmContentStorage* storage, Storage where, const NcmContentId& id, i64 offset, void* buffer, size_t count) {
        constexpr size_t Chunk = 8 << 20;
        sys::giveWay(count);
        std::shared_ptr<OpenFile> file = find(storage, where, id);
        u8* out = (u8*)buffer;
        while (count > 0) {
            size_t n = std::min(count, Chunk);
            u64 got = 0;
            if (!file || R_FAILED(fsFileRead(&file->file, offset, out, n, FsReadOption_None, &got)) || got != n) {
                file = nullptr;
                check(ncmContentStorageReadContentIdFile(storage, out, n, &id, offset), "无法读取已安装的游戏");
            }
            out += n;
            offset += (i64)n;
            count -= n;
        }
    }

private:
    struct OpenFile {
        FsFile file;
        NcmContentId id;
        Storage where;
        ~OpenFile() { fsFileClose(&file); }
    };

    struct OneStorage {
        FsFileSystem fs;
        bool tried = false;
        bool mounted = false;
    };

    std::shared_ptr<OpenFile> find(NcmContentStorage* storage, Storage where, const NcmContentId& id) {
        constexpr size_t MostOpen = 8;
        sys::Lock lock(_mutex);
        for (auto& file : _open) {
            if (file->where == where && memcmp(file->id.c, id.c, sizeof(id.c)) == 0)
                return file;
        }

        OneStorage& mine = _storages[where == Storage::Sd ? 0 : 1];
        if (!mine.tried) {
            mine.tried = true;
            mine.mounted = R_SUCCEEDED(fsOpenContentStorageFileSystem(&mine.fs, where == Storage::Sd ? FsContentStorageId_SdCard : FsContentStorageId_User));
        }
        if (!mine.mounted)
            return nullptr;

        // What's said here starts with where the content manager has it mounted, and the part after that goes for here as well.
        char path[FS_MAX_PATH] = {};
        if (R_FAILED(ncmContentStorageGetPath(storage, path, sizeof(path), &id)))
            return nullptr;
        const char* within = strstr(path, "registered/");
        if (!within)
            return nullptr;
        std::string inner = std::string("/") + within;
        if (inner.back() == '/')
            inner.pop_back();

        FsFile opened;
        if (R_FAILED(fsFsOpenFile(&mine.fs, inner.c_str(), FsOpenMode_Read, &opened))) {
            // Not worth trying again for every read of everything.
            sink().message("正在通过内容管理器读取" + std::string(storageName(where)));
            fsFsClose(&mine.fs);
            mine.mounted = false;
            return nullptr;
        }
        auto file = std::make_shared<OpenFile>();
        file->file = opened;
        file->id = id;
        file->where = where;
        if (_open.size() >= MostOpen)
            _open.erase(_open.begin());
        _open.push_back(file);
        return file;
    }

    sys::Mutex _mutex;
    OneStorage _storages[2];
    std::vector<std::shared_ptr<OpenFile>> _open;
};

class NxContent : public DataSource {
public:
    NxContent(DirectReader& reader, NcmContentStorage* storage, Storage where, const ContentId& id, i64 size) : _reader(reader), _storage(storage), _where(where), _id(toNcm(id)), _size(size) {}

    i64 length() const override { return _size; }
    bool readsAtOnce() const override { return true; }
    void read(i64 offset, void* buffer, size_t count) override { _reader.read(_storage, _where, _id, offset, buffer, count); }

private:
    DirectReader& _reader;
    NcmContentStorage* _storage;
    Storage _where;
    NcmContentId _id;
    i64 _size;
};

class NxWriter : public ContentWriter {
public:
    NxWriter(DirectReader& reader, NcmContentStorage* storage, i64 size) : _reader(reader), _storage(storage), _exact(size >= 0), _size(size >= 0 ? size : GrowBy) {
        check(ncmContentStorageGeneratePlaceHolderId(_storage, &_placeholder), "无法为新副本分配空间");
        // The content ID that goes with a placeholder only says which folder to make sure of, and the real one isn't known yet.
        NcmContentId unknown;
        memcpy(unknown.c, _placeholder.uuid.uuid, sizeof(unknown.c));
        Result rc = ncmContentStorageCreatePlaceHolder(_storage, &unknown, &_placeholder, _size);
        if (R_FAILED(rc) && !_exact) {
            _size = 0;
            rc = ncmContentStorageCreatePlaceHolder(_storage, &unknown, &_placeholder, 0);
        }
        check(rc, "无法为新副本分配空间");
        _created = true;
    }

    ~NxWriter() override {
        if (_created)
            ncmContentStorageDeletePlaceHolder(_storage, &_placeholder);
    }

    std::string token() const override { return toHex(_placeholder.uuid.uuid, sizeof(_placeholder.uuid.uuid)); }

    void write(i64 offset, const void* data, size_t size) override {
        constexpr size_t Chunk = 8 << 20;
        sys::giveWay(size);
        i64 end = offset + (i64)size;
        if (end > _size)
            grow(end);
        const u8* in = (const u8*)data;
        while (size > 0) {
            size_t n = std::min(size, Chunk);
            check(ncmContentStorageWritePlaceHolder(_storage, &_placeholder, (u64)offset, in, n), "无法写入新副本");
            in += n;
            offset += (i64)n;
            size -= n;
        }
        _end = std::max(_end, end);
    }

    void setSize(i64 size) override {
        flushWrites();
        check(ncmContentStorageSetPlaceHolderSize(_storage, &_placeholder, size), "无法完成新副本的写入");
        _size = size;
    }

    void holdAs(const ContentId& temporary) override {
        NcmContentId content = toNcm(temporary);
        registerAs(content);
        _held = content;
        _isHeld = true;
    }

    void finishAs(const ContentId& id) override {
        NcmContentId content = toNcm(id);
        if (_isHeld) {
            // It's just been read back, and a file that's open can't change its name.
            _reader.closeAll();
            check(ncmContentStorageRevertToPlaceHolder(_storage, &_placeholder, &_held, &content), "无法完成新副本的写入");
            _isHeld = false;
        } else if (_end != _size) {
            throw ToolError("内部错误：实际写入量小于占位预留量");
        }
        registerAs(content);
    }

private:
    /// It grows in large steps, so that it stays in as few pieces as possible.
    static constexpr i64 GrowBy = 1LL << 30;

    void grow(i64 end) {
        if (_exact)
            throw ToolError("内部错误：实际写入量大于占位预留量");
        i64 wanted = std::max(end, _size + GrowBy);
        flushWrites();
        Result rc = ncmContentStorageSetPlaceHolderSize(_storage, &_placeholder, wanted);
        if (R_FAILED(rc) && wanted > end) {
            // Near the end of what's free, it can still fit if it doesn't ask for more than it needs.
            wanted = end;
            rc = ncmContentStorageSetPlaceHolderSize(_storage, &_placeholder, wanted);
        }
        if (R_FAILED(rc))
            throw ToolError(format("There isn't enough room to write the new copy (%s)", codeOf(rc).c_str()));
        _size = wanted;
    }

    /// On the console's own storage, ncm keeps what's been written in its open file until told to flush it. Changing the size
    /// or the name closes that file, and closing it with writes not yet flushed brings the whole console down (2002-6454).
    void flushWrites() {
        if (hosversionAtLeast(3, 0, 0))
            ncmContentStorageFlushPlaceHolder(_storage);
    }

    void registerAs(const NcmContentId& content) {
        // Registering it moves it into the folder that goes with its ID, and only making a placeholder for that ID makes sure the folder's there.
        NcmPlaceHolderId temporary;
        check(ncmContentStorageGeneratePlaceHolderId(_storage, &temporary), "无法完成新副本的写入");
        check(ncmContentStorageCreatePlaceHolder(_storage, &content, &temporary, 0x10), "无法完成新副本的写入");
        ncmContentStorageDeletePlaceHolder(_storage, &temporary);

        flushWrites();
        check(ncmContentStorageRegister(_storage, &content, &_placeholder), "无法完成新副本的写入");
        _created = false;
    }

    DirectReader& _reader;
    NcmContentStorage* _storage;
    NcmPlaceHolderId _placeholder;
    bool _exact;
    i64 _size;
    i64 _end = 0;
    bool _created = false;
    NcmContentId _held;
    bool _isHeld = false;
};

/// A mounted NCA, read with the firmware's own code.
class NxView : public FirmwareView {
public:
    explicit NxView(FsFileSystem fs) : _fs(fs) {}
    ~NxView() override { fsFsClose(&_fs); }

    void read(const std::string& path, i64 offset, void* buffer, size_t size) override {
        FsFile file;
        check(fsFsOpenFile(&_fs, path.c_str(), FsOpenMode_Read, &file), "固件无法打开新副本中的文件");
        u64 got = 0;
        Result rc = fsFileRead(&file, offset, buffer, size, FsReadOption_None, &got);
        fsFileClose(&file);
        check(rc, "固件无法读取新副本中的文件");
        if (got != size)
            throw ToolError("固件返回的文件内容少于请求长度");
    }

private:
    FsFileSystem _fs;
};

class NxConsole : public ConsoleApi {
public:
    NxConsole() {
        check(ncmInitialize(), "内容管理器无法启动");
        _ncm = true;
        check(nsInitialize(), "游戏列表服务无法启动");
        _ns = true;
        check(nsGetApplicationManagerInterface(&_manager), "游戏列表服务无法启动");
        _managerOpen = true;

        for (Storage storage : {Storage::Sd, Storage::Nand}) {
            auto& side = _sides[indexOf(storage)];
            side.storageOpen = R_SUCCEEDED(ncmOpenContentStorage(&side.storage, storageIdOf(storage)));
            side.databaseOpen = R_SUCCEEDED(ncmOpenContentMetaDatabase(&side.database, storageIdOf(storage)));
        }
        if (!_sides[indexOf(Storage::Nand)].storageOpen)
            throw ToolError("无法打开主机内置存储");
        // Worked out now, as games get read on several threads at once.
        language();
    }

    ~NxConsole() override {
        _reader.reset();
        for (auto& side : _sides) {
            if (side.storageOpen)
                ncmContentStorageClose(&side.storage);
            if (side.databaseOpen)
                ncmContentMetaDatabaseClose(&side.database);
        }
        if (_esOpen)
            serviceClose(&_es);
        if (_managerOpen)
            serviceClose(&_manager);
        if (_ns)
            nsExit();
        if (_ncm)
            ncmExit();
    }

    std::vector<u64> listApplications() override {
        constexpr int Batch = 64;
        std::vector<u64> list;
        std::vector<NsApplicationRecord> records(Batch);
        for (s32 offset = 0;;) {
            s32 count = 0;
            check(nsListApplicationRecord(records.data(), Batch, offset, &count), "无法列出此主机上的游戏");
            for (s32 i = 0; i < count; i++)
                list.push_back(records[(size_t)i].application_id);
            if (count < Batch)
                break;
            offset += count;
        }
        return list;
    }

    InstalledGame describe(u64 applicationId) override {
        sys::giveWay();
        InstalledGame game;
        game.applicationId = applicationId;

        for (auto& record : records(applicationId)) {
            InstalledMeta meta;
            meta.key = record.first;
            meta.storage = record.second;
            if (meta.storage == Storage::GameCard) {
                game.metas.push_back(meta);
            } else if (readContents(meta)) {
                game.metas.push_back(meta);
            }
        }

        u64 self = 0, running = 0, process = 0;
        svcGetInfo(&self, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0);
        if (R_SUCCEEDED(pmdmntGetApplicationProcessId(&process)))
            pminfoGetProgramId(&running, process);
        if (applicationId == self)
            game.blocked = "本程序是通过该游戏启动的，请改用其他游戏启动本程序后再压缩它";
        else if (applicationId == running)
            game.blocked = "该游戏当前处于打开状态，请关闭后再压缩";
        return game;
    }

    void readControl(InstalledGame& game) override {
        sys::giveWay();
        // The service for this can take half a second a game, which is a long wait with hundreds of them. Going straight to the game's own files
        // takes a fraction of that. The newest is what the home menu goes by, so an update comes first.
        for (u8 type : {Cnmt::MetaPatch, Cnmt::MetaApplication}) {
            for (auto& meta : game.metas) {
                if (meta.key.type != type)
                    continue;
                for (auto& content : meta.contents) {
                    if (content.type == Cnmt::ContentTypeControl && readControlFiles(game, meta.storage, meta.key.id, content))
                        return;
                }
            }
        }
        readControlFromService(game);
    }

    SourcePtr openContent(Storage storage, const ContentId& id, i64 size) override { return std::make_shared<NxContent>(*_reader, contentStorage(storage), storage, id, size); }

    void addTitleKeys(TicketStore& store, const Keyset& keyset) override {
        loadTickets(keyset);
        for (auto& ticket : _tickets)
            store.addTicket(ticket.data(), ticket.size());
        for (auto& key : _personalKeys)
            store.addEncryptedKey(key.first, key.second.data());
    }

    i64 freeBytes(Storage storage) override {
        auto& side = _sides[indexOf(storage)];
        s64 size = 0;
        return side.storageOpen && R_SUCCEEDED(ncmContentStorageGetFreeSpaceSize(&side.storage, &size)) ? size : 0;
    }

    i64 totalBytes(Storage storage) override {
        auto& side = _sides[indexOf(storage)];
        s64 size = 0;
        return side.storageOpen && R_SUCCEEDED(ncmContentStorageGetTotalSpaceSize(&side.storage, &size)) ? size : 0;
    }

    std::unique_ptr<ContentWriter> createContent(Storage storage, i64 size) override { return std::make_unique<NxWriter>(*_reader, contentStorage(storage), size); }

    void deletePlaceholder(Storage storage, const std::string& token) override {
        Bytes bytes;
        NcmPlaceHolderId placeholder;
        if (!fromHex(token, bytes) || bytes.size() != sizeof(placeholder.uuid.uuid))
            return;
        memcpy(placeholder.uuid.uuid, bytes.data(), bytes.size());
        ncmContentStorageDeletePlaceHolder(contentStorage(storage), &placeholder);
    }

    bool hasContent(Storage storage, const ContentId& id) override {
        NcmContentId content = toNcm(id);
        bool has = false;
        check(ncmContentStorageHas(contentStorage(storage), &has, &content), "无法查找内容");
        return has;
    }

    void deleteContent(Storage storage, const ContentId& id) override {
        NcmContentId content = toNcm(id);
        // A file that's open can't be deleted.
        _reader->closeAll();
        check(ncmContentStorageDelete(contentStorage(storage), &content), "无法删除旧副本");
    }

    void setMeta(Storage storage, const MetaKey& key, const Bytes& installMeta) override {
        NcmContentMetaKey ncm = toNcm(key);
        NcmContentMetaDatabase* database = metaDatabase(storage);
        check(ncmContentMetaDatabaseSet(database, &ncm, installMeta.data(), installMeta.size()), "无法登记新副本");
        check(ncmContentMetaDatabaseCommit(database), "无法登记新副本");
    }

    bool hasMeta(Storage storage, const MetaKey& key) override {
        NcmContentMetaKey ncm = toNcm(key);
        bool has = false;
        check(ncmContentMetaDatabaseHas(metaDatabase(storage), &has, &ncm), "无法查找游戏记录");
        return has;
    }

    void removeMeta(Storage storage, const MetaKey& key) override {
        NcmContentMetaKey ncm = toNcm(key);
        NcmContentMetaDatabase* database = metaDatabase(storage);
        check(ncmContentMetaDatabaseRemove(database, &ncm), "无法移除旧副本的记录");
        check(ncmContentMetaDatabaseCommit(database), "无法移除旧副本的记录");
    }

    void setLaunchVersion(u64 applicationId, u32 version) override {
        check(avmInitialize(), "相关服务无法启动");
        Result rc = avmPushLaunchVersion(applicationId, version);
        avmExit();
        check(rc, "对方未接受");
    }

    void setRecords(u64 applicationId, const std::vector<std::pair<MetaKey, Storage>>& list) override {
        // It isn't there the first time a game gets installed, and that's fine.
        nsextDeleteRecords(&_manager, applicationId);
        if (list.empty())
            return;
        std::vector<NsextRecord> records;
        for (auto& item : list) {
            NsextRecord record = {};
            record.key = toNcm(item.first);
            record.storage = (u64)storageIdOf(item.second);
            records.push_back(record);
        }
        check(nsextPushRecords(&_manager, applicationId, records.data(), (s32)records.size()), "无法把新副本通知主菜单");
    }

    std::vector<std::pair<MetaKey, Storage>> records(u64 applicationId) override {
        constexpr int Batch = 64;
        std::vector<std::pair<MetaKey, Storage>> list;
        std::vector<NsextRecord> batch(Batch);
        for (s32 offset = 0;;) {
            s32 count = 0;
            // A game that isn't installed has no records, which comes back as an error.
            if (R_FAILED(nsextListRecords(&_manager, applicationId, offset, batch.data(), Batch, &count)))
                break;
            for (s32 i = 0; i < count; i++) {
                const NsextRecord& record = batch[(size_t)i];
                MetaKey key;
                key.id = record.key.id;
                key.version = record.key.version;
                key.type = record.key.type;
                key.installType = record.key.install_type;
                Storage storage = record.storage == NcmStorageId_SdCard ? Storage::Sd : record.storage == NcmStorageId_BuiltInUser ? Storage::Nand : record.storage == NcmStorageId_GameCard ? Storage::GameCard : Storage::None;
                list.emplace_back(key, storage);
            }
            if (count < Batch)
                break;
            offset += count;
        }
        return list;
    }

    std::unique_ptr<FirmwareView> mountGameData(Storage storage, const ContentId& id, u64 programId) override { return mount(storage, id, programId, FsFileSystemType_ContentData); }
    std::unique_ptr<FirmwareView> mountLogo(Storage storage, const ContentId& id, u64 programId) override { return mount(storage, id, programId, FsFileSystemType_Logo); }

private:
    struct Side {
        NcmContentStorage storage;
        NcmContentMetaDatabase database;
        bool storageOpen = false;
        bool databaseOpen = false;
    };

    static int indexOf(Storage storage) { return storage == Storage::Sd ? 0 : 1; }

    NcmContentStorage* contentStorage(Storage storage) {
        if (storage != Storage::Sd && storage != Storage::Nand)
            throw ToolError(format("Can't work with what's on %s", storageName(storage)));
        auto& side = _sides[indexOf(storage)];
        if (!side.storageOpen)
            throw ToolError(format("Couldn't open %s", storageName(storage)));
        return &side.storage;
    }

    NcmContentMetaDatabase* metaDatabase(Storage storage) {
        if (storage != Storage::Sd && storage != Storage::Nand)
            throw ToolError(format("Can't work with what's on %s", storageName(storage)));
        auto& side = _sides[indexOf(storage)];
        if (!side.databaseOpen)
            throw ToolError(format("Couldn't open the records of %s", storageName(storage)));
        return &side.database;
    }

    /// Which of a NACP's languages the console is set to, in the NACP's own order.
    int language() {
        if (_language != -2)
            return _language;
        _language = -1;
        if (R_FAILED(setInitialize()))
            return _language;
        u64 code = 0;
        SetLanguage chosen;
        if (R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &chosen))) {
            // In the order of SetLanguage: Japanese, US English, French, German, Italian, Spanish, Chinese, Korean, Dutch, Portuguese, Russian,
            // Taiwanese, British English, Canadian French, Latin American Spanish, Simplified Chinese, Traditional Chinese, Brazilian Portuguese.
            static const signed char order[] = {2, 0, 3, 4, 7, 6, 14, 12, 8, 10, 11, 13, 1, 9, 5, 14, 13, 15};
            if ((size_t)chosen < sizeof(order))
                _language = order[chosen];
        }
        setExit();
        return _language;
    }

    /// The names and version out of a control.nacp. False if there's no name in it.
    bool takeNames(InstalledGame& game, const u8* nacp, size_t size) {
        NacpTitle title = Nacp::choose(Nacp::titles(nacp, size), language());
        if (title.name.empty())
            return false;
        game.name = title.name;
        game.publisher = title.publisher;
        game.displayVersion = Nacp::displayVersion(nacp, size);
        return true;
    }

    bool readFileFrom(FsFileSystem& fs, const char* path, Bytes& out, size_t limit) {
        FsFile file;
        if (R_FAILED(fsFsOpenFile(&fs, path, FsOpenMode_Read, &file)))
            return false;
        s64 size = 0;
        bool ok = false;
        if (R_SUCCEEDED(fsFileGetSize(&file, &size)) && size > 0 && (size_t)size <= limit) {
            out.resize((size_t)size);
            u64 got = 0;
            ok = R_SUCCEEDED(fsFileRead(&file, 0, out.data(), out.size(), FsReadOption_None, &got)) && got == out.size();
        }
        fsFileClose(&file);
        return ok;
    }

    bool readControlFiles(InstalledGame& game, Storage storage, u64 titleId, const InstalledContent& content) {
        if (storage != Storage::Sd && storage != Storage::Nand)
            return false;
        NcmContentId id = toNcm(content.id);
        char path[FS_MAX_PATH] = {};
        if (R_FAILED(ncmContentStorageGetPath(contentStorage(storage), path, sizeof(path), &id)))
            return false;
        FsFileSystem fs;
        // An update's control data can go by the game's ID or its own.
        if (R_FAILED(fsOpenFileSystemWithId(&fs, game.applicationId, FsFileSystemType_ContentControl, path, FsContentAttributes_All)) && R_FAILED(fsOpenFileSystemWithId(&fs, titleId, FsFileSystemType_ContentControl, path, FsContentAttributes_All)))
            return false;

        Bytes nacp;
        bool named = readFileFrom(fs, "/control.nacp", nacp, Nacp::Size) && takeNames(game, nacp.data(), nacp.size());
        if (named)
            readIconFrom(fs, game);
        fsFsClose(&fs);
        return named;
    }

    void readIconFrom(FsFileSystem& fs, InstalledGame& game) {
        constexpr size_t MostIcon = 0x20000;
        static const char* const names[] = {"AmericanEnglish", "BritishEnglish", "Japanese", "French", "German", "LatinAmericanSpanish", "Spanish", "Italian", "Dutch", "CanadianFrench", "Portuguese", "Russian", "Korean", "TraditionalChinese", "SimplifiedChinese", "BrazilianPortuguese", "Polish", "Thai"};
        constexpr int NameCount = sizeof(names) / sizeof(names[0]);
        int preferred = language();
        char path[64];
        if (preferred >= 0 && preferred < NameCount) {
            snprintf(path, sizeof(path), "/icon_%s.dat", names[preferred]);
            if (readFileFrom(fs, path, game.icon, MostIcon))
                return;
        }
        for (const char* name : names) {
            snprintf(path, sizeof(path), "/icon_%s.dat", name);
            if (readFileFrom(fs, path, game.icon, MostIcon))
                return;
        }
    }

    /// The slow way, which is also the only way for what isn't on the SD card or the console itself, and for a Switch 2 Edition, whose name can be
    /// in one of several control sets.
    void readControlFromService(InstalledGame& game) {
        auto control = std::make_unique<NsApplicationControlData>();
        auto take = [&](u64 actual) {
            if (actual < sizeof(control->nacp) || !takeNames(game, (const u8*)&control->nacp, sizeof(control->nacp)))
                return false;
            game.icon.assign(control->icon, control->icon + (actual - sizeof(control->nacp)));
            return true;
        };
        u64 actual = 0;
        if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage, game.applicationId, control.get(), sizeof(*control), &actual)) && take(actual))
            return;
        if (!hosversionAtLeast(19, 0, 0))
            return;
        for (u8 set = 1; set <= 3; set++) {
            u32 unused = 0;
            if (R_SUCCEEDED(nsGetApplicationControlData2(NsApplicationControlSource_Storage, game.applicationId, control.get(), sizeof(*control), 0, set, &actual, &unused)) && take(actual))
                return;
        }
    }

    /// False if it isn't actually on this console, as with a game the system has archived.
    bool readContents(InstalledMeta& meta) {
        NcmContentMetaKey key = toNcm(meta.key);
        Storage order[] = {meta.storage, Storage::Sd, Storage::Nand};
        for (Storage storage : order) {
            if (storage != Storage::Sd && storage != Storage::Nand)
                continue;
            auto& side = _sides[indexOf(storage)];
            bool has = false;
            if (!side.databaseOpen || R_FAILED(ncmContentMetaDatabaseHas(&side.database, &has, &key)) || !has)
                continue;

            constexpr int Batch = 32;
            NcmContentInfo infos[Batch];
            for (s32 start = 0;;) {
                s32 written = 0;
                check(ncmContentMetaDatabaseListContentInfo(&side.database, &written, infos, Batch, &key, start), "无法读取游戏记录");
                for (s32 i = 0; i < written; i++) {
                    InstalledContent content;
                    memcpy(content.id.bytes, infos[i].content_id.c, sizeof(content.id.bytes));
                    ncmContentInfoSizeToU64(&infos[i], (u64*)&content.size);
                    // Sphaira's forwarders have the size of the file inside it recorded for this one, and what's stored is all there.
                    s64 stored = 0;
                    if (content.type == NcmContentType_Meta && content.size < 0xC00 && side.storageOpen && R_SUCCEEDED(ncmContentStorageGetSizeFromContentId(&side.storage, &stored, &infos[i].content_id)))
                        content.size = stored;
                    content.type = infos[i].content_type;
                    content.idOffset = infos[i].id_offset;
                    meta.contents.push_back(content);
                }
                if (written < Batch)
                    break;
                start += written;
            }
            meta.storage = storage;
            return true;
        }
        return false;
    }

    std::unique_ptr<FirmwareView> mount(Storage storage, const ContentId& id, u64 programId, FsFileSystemType type) {
        NcmContentId content = toNcm(id);
        char path[FS_MAX_PATH] = {};
        check(ncmContentStorageGetPath(contentStorage(storage), path, sizeof(path), &content), "找不到新副本");
        FsFileSystem fs;
        check(fsOpenFileSystemWithId(&fs, programId, type, path, FsContentAttributes_All), "固件拒绝挂载");
        return std::make_unique<NxView>(fs);
    }

    /// Every ticket on the console, once: they don't change while this runs, and there can be hundreds.
    void loadTickets(const Keyset& keyset) {
        if (_ticketsLoaded)
            return;
        _ticketsLoaded = true;

        if (R_FAILED(smGetService(&_es, "es")))
            throw ToolError("游戏票据服务无法启动");
        _esOpen = true;

        constexpr size_t RightsIdSize = 0x10;
        constexpr size_t TicketCapacity = 0x400;
        for (bool personalized : {false, true}) {
            u32 count = 0;
            if (R_FAILED(esextCount(&_es, personalized, &count)) || count == 0)
                continue;
            Bytes rightsIds(count * RightsIdSize);
            u32 written = 0;
            if (R_FAILED(esextList(&_es, personalized, &written, rightsIds.data(), rightsIds.size())))
                continue;
            for (u32 i = 0; i < written && i < count; i++) {
                Bytes ticket(TicketCapacity);
                u64 size = 0;
                const u8* rightsId = rightsIds.data() + i * RightsIdSize;
                if (R_FAILED(esextGet(&_es, personalized, rightsId, &size, ticket.data(), ticket.size())) || size == 0 || size > ticket.size())
                    continue;
                ticket.resize((size_t)size);
                if (!personalized) {
                    _tickets.push_back(std::move(ticket));
                } else {
                    // Noted as there, even if the key to it can't be worked out, for the sake of what gets said about it then.
                    _tickets.push_back(ticket);
                    addPersonalKey(ticket, rightsId, keyset);
                }
            }
        }
    }

    /// A personalized ticket's key is encrypted with this console's own RSA key, itself encrypted with a key from prod.keys.
    void addPersonalKey(const Bytes& ticket, const u8* rightsId, const Keyset& keyset) {
        constexpr size_t TitleKeyBlock = 0x180;
        if (ticket.size() < TitleKeyBlock + 0x100 || !loadDeviceKey(keyset))
            return;
        u8 block[0x100];
        rsaPrivate(_deviceModulus.data(), _deviceExponent.data(), ticket.data() + TitleKeyBlock, block);
        Bytes message;
        if (oaepUnpad(block, message) && message.size() == 16) {
            std::array<u8, 16> key;
            memcpy(key.data(), message.data(), 16);
            _personalKeys.emplace_back(toHex(rightsId, 0x10), key);
        }
    }

    bool loadDeviceKey(const Keyset& keyset) {
        if (_deviceKeyTried)
            return !_deviceModulus.empty();
        _deviceKeyTried = true;

        if (R_FAILED(setcalInitialize()))
            return false;
        auto raw = std::make_unique<SetCalRsa2048DeviceKey>();
        Result rc = setcalGetEticketDeviceKey(raw.get());
        setcalExit();
        if (R_FAILED(rc))
            return false;

        // A counter, then the exponent, modulus and public exponent, encrypted with one of two keys. The right one gives a public exponent that makes sense.
        constexpr size_t PayloadSize = 0x100 + 0x100 + 4;
        for (const char* name : {"eticket_rsa_kek", "eticket_rsa_kek_personalized"}) {
            const u8* kek = keyset.find(name, 16);
            if (!kek)
                continue;
            Bytes plain(PayloadSize);
            Aes128CtrContext context;
            aes128CtrContextCreate(&context, kek, raw->key);
            aes128CtrCrypt(&context, plain.data(), raw->key + 0x10, PayloadSize);
            if (plain[0x200] == 0 && plain[0x201] == 1 && plain[0x202] == 0 && plain[0x203] == 1) {
                _deviceExponent.assign(plain.begin(), plain.begin() + 0x100);
                _deviceModulus.assign(plain.begin() + 0x100, plain.begin() + 0x200);
                return true;
            }
        }
        return false;
    }

    std::unique_ptr<DirectReader> _reader = std::make_unique<DirectReader>();
    bool _ncm = false;
    bool _ns = false;
    bool _managerOpen = false;
    Service _manager;
    bool _esOpen = false;
    Service _es;
    Side _sides[2];

    int _language = -2;
    bool _ticketsLoaded = false;
    std::vector<Bytes> _tickets;
    std::vector<std::pair<std::string, std::array<u8, 16>>> _personalKeys;
    bool _deviceKeyTried = false;
    Bytes _deviceExponent;
    Bytes _deviceModulus;
};

}  // namespace

std::unique_ptr<ConsoleApi> openConsole(std::string& problem) {
    try {
        return std::make_unique<NxConsole>();
    } catch (const ToolError& e) {
        problem = e.what();
        return nullptr;
    }
}

}  // namespace hbp
