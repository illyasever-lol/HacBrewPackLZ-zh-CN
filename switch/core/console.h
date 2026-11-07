// What the engine needs from a console to work with installed games. nx_console.cpp does this with the system's own services,
// and host/sim_console.cpp pretends to, so the whole of replacing a game can be tested on a PC, power cuts included.
#pragma once

#include "title.h"

namespace hbp {

enum class Storage : u8 { None = 0, GameCard = 2, Nand = 4, Sd = 5 };

const char* storageName(Storage storage);

struct ContentId {
    u8 bytes[16] = {};
    std::string hex() const { return toHex(bytes, 16); }
    bool operator==(const ContentId& other) const { return memcmp(bytes, other.bytes, 16) == 0; }
    bool operator<(const ContentId& other) const { return memcmp(bytes, other.bytes, 16) < 0; }
    static bool parse(const std::string& hex, ContentId& out);
};

struct MetaKey {
    u64 id = 0;
    u32 version = 0;
    u8 type = 0;
    u8 installType = 0;
    bool operator==(const MetaKey& o) const { return id == o.id && version == o.version && type == o.type; }
};

struct InstalledContent {
    ContentId id;
    i64 size = 0;
    u8 type = 0;
    u8 idOffset = 0;
};

struct InstalledMeta {
    MetaKey key;
    Storage storage = Storage::None;
    std::vector<InstalledContent> contents;
};

struct InstalledGame {
    u64 applicationId = 0;
    std::string name;
    std::string publisher;
    std::string displayVersion;
    /// As stored with the game, a 256x256 JPEG.
    Bytes icon;
    std::vector<InstalledMeta> metas;
    /// Why this game can't be touched just now, if it can't.
    std::string blocked;

    i64 totalBytes() const;
};

/// Bytes on their way into the console's storage. They only count once registered, and go away if not.
class ContentWriter {
public:
    virtual ~ContentWriter() = default;
    /// What it takes to find these bytes again after a power cut.
    virtual std::string token() const = 0;
    /// It grows as needed, if there's room.
    virtual void write(i64 offset, const void* data, size_t size) = 0;
    virtual void setSize(i64 size) = 0;
    /// Registered under an ID that isn't the real one, as it can only be read once it's registered, and its real one comes from reading it.
    virtual void holdAs(const ContentId& temporary) = 0;
    virtual void finishAs(const ContentId& id) = 0;
};

/// A file in a mounted NCA, as the firmware itself reads it.
class FirmwareView {
public:
    virtual ~FirmwareView() = default;
    virtual void read(const std::string& path, i64 offset, void* buffer, size_t size) = 0;
};

class ConsoleApi {
public:
    virtual ~ConsoleApi() = default;

    virtual std::vector<u64> listApplications() = 0;
    /// What's installed of a game. That's quick, unlike its name and icon, which come from readControl.
    virtual InstalledGame describe(u64 applicationId) = 0;
    /// Fills in the name, the publisher, the version people know it by and the icon.
    virtual void readControl(InstalledGame& game) = 0;
    virtual SourcePtr openContent(Storage storage, const ContentId& id, i64 size) = 0;
    /// Puts the title key of every ticket the console holds in the store. The keys are for working out personalized ones.
    virtual void addTitleKeys(TicketStore& store, const Keyset& keyset) = 0;

    virtual i64 freeBytes(Storage storage) = 0;
    virtual i64 totalBytes(Storage storage) = 0;

    /// The size is negative if it isn't known yet.
    virtual std::unique_ptr<ContentWriter> createContent(Storage storage, i64 size) = 0;
    virtual void deletePlaceholder(Storage storage, const std::string& token) = 0;
    virtual bool hasContent(Storage storage, const ContentId& id) = 0;
    virtual void deleteContent(Storage storage, const ContentId& id) = 0;
    /// Written and committed before this returns.
    virtual void setMeta(Storage storage, const MetaKey& key, const Bytes& installMeta) = 0;
    virtual bool hasMeta(Storage storage, const MetaKey& key) = 0;
    virtual void removeMeta(Storage storage, const MetaKey& key) = 0;
    /// What the home menu goes by to find a game's content.
    virtual void setRecords(u64 applicationId, const std::vector<std::pair<MetaKey, Storage>>& records) = 0;
    virtual std::vector<std::pair<MetaKey, Storage>> records(u64 applicationId) = 0;
    /// The console remembers how new a game has to be to start, and says to update it otherwise. This says what's there now is what counts.
    virtual void setLaunchVersion(u64 applicationId, u32 version) = 0;

    /// Mounts a Data NCA with the firmware's own code, which is what will have to read it later. Throws if the firmware won't have it.
    virtual std::unique_ptr<FirmwareView> mountGameData(Storage storage, const ContentId& id, u64 programId) = 0;
    /// The firmware won't mount the game data of a Program NCA from outside a game that's running, but it will its logo.
    virtual std::unique_ptr<FirmwareView> mountLogo(Storage storage, const ContentId& id, u64 programId) = 0;
};

/// The same as what's mounted above, but read by this program's own code.
std::unique_ptr<FirmwareView> readGameDataOf(const SourcePtr& raw, const std::string& name, const Keyset& keyset);
std::unique_ptr<FirmwareView> readLogoOf(const SourcePtr& raw, const std::string& name, const Keyset& keyset);

/// The contents of an installed game, seen as if it were a game file.
class InstalledArchive : public Archive {
public:
    InstalledArchive(ConsoleApi& console, const InstalledGame& game);
    SourcePtr open(const Entry& entry) override;

private:
    ConsoleApi& _console;
    std::vector<std::pair<Storage, ContentId>> _where;
};

struct ReplaceResult {
    i64 before = 0;
    i64 after = 0;
};

/// Replaces an installed title with its compressed self, and never leaves a game unplayable along the way. The new copy is written beside
/// the old one and checked, unless it's been asked not to be: every byte against what was written, and game data from across it against the
/// old copy's, read by the firmware where it can be asked to. Only then is the switch made, and it's written down first, so a power cut at any
/// point can be finished or undone the next time this runs. The old copy is what goes last.
class Replacer {
public:
    Replacer(ConsoleApi& console, std::string journalFolder) : _console(console), _journalFolder(std::move(journalFolder)) {}

    /// Whether this console can read compressed games is best found out before any time goes on one, and only takes finding out the once.
    /// A small test copy is written, read back through the firmware's own code, and taken away again. Throws if that doesn't work.
    void checkConsole(Storage storage, const ConvertOptions& options, const Keyset& keyset);

    /// A title that isn't installed yet goes to installTo.
    ReplaceResult run(const InstalledGame& game, const TitleJob& job, const ConvertOptions& options, const Keyset& keyset, Storage installTo = Storage::None);
    /// The same, with what's in the folder of an extracted game.
    ReplaceResult run(const InstalledGame& game, const ExtractedGame& extracted, const ConvertOptions& options, const Keyset& keyset, Storage installTo);

    /// Finishes or undoes whatever a power cut interrupted. Call this before anything else. Returns what it had to do.
    std::vector<std::string> recover();

private:
    ConsoleApi& _console;
    std::string _journalFolder;
};

/// The content meta as the console's database holds it, made from the one in a title.
Bytes installMetaFrom(const ContentMeta& rebuilt, const NcaInfo& metaNca);

}  // namespace hbp
