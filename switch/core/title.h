// Game files and titles: what a file or an installed game holds, which update goes with which base game, and what it takes to
// rebuild a title, estimate it or extract it.
#pragma once

#include <map>

#include "nacp.h"
#include "nca.h"
#include "ncaout.h"
#include "source.h"

namespace hbp {

namespace Cnmt {
constexpr u8 ContentTypeMeta = 0;
constexpr u8 ContentTypeProgram = 1;
constexpr u8 ContentTypeData = 2;
constexpr u8 ContentTypeControl = 3;
constexpr u8 ContentTypeHtmlDocument = 4;
constexpr u8 ContentTypeLegalInformation = 5;
constexpr u8 ContentTypeDeltaFragment = 6;

constexpr u8 MetaApplication = 0x80;
constexpr u8 MetaPatch = 0x81;
constexpr u8 MetaAddOnContent = 0x82;

std::string fileName(u64 titleId, u8 metaType);
}  // namespace Cnmt

struct ContentMetaRecord {
    std::string id;
    i64 size;
    u8 type;
    u8 idOffset;
};

struct ContentRecord {
    NcaInfo nca;
    u8 type;
    u8 idOffset;
};

/// A parsed content meta file.
struct ContentMeta {
    Bytes raw;
    u64 titleId = 0;
    u32 version = 0;
    u8 type = 0;
    int extendedHeaderSize = 0;
    std::vector<ContentMetaRecord> records;

    static ContentMeta parse(Bytes raw, const std::string& what);

    /// For an update or a DLC, the title it belongs to. For a base game, its update's title ID.
    u64 linkedId() const { return extendedHeaderSize >= 8 ? rd64(&raw[0x20]) : 0; }
    /// Only base games and updates have this field.
    u32 requiredSystemVersion() const {
        return (type == Cnmt::MetaApplication || type == Cnmt::MetaPatch) && extendedHeaderSize >= 0xC ? rd32(&raw[0x28]) : 0;
    }
    int rebuiltSize(int recordCount) const { return 0x20 + extendedHeaderSize + recordCount * 0x38 + 0x20; }
    /// A copy of this content meta with a new version and new content records. Required version 0 keeps what's there.
    Bytes rebuild(u32 version, u32 requiredSystemVersion, const std::vector<ContentRecord>& records) const;
};

/// A game file opened for reading, or the contents of an installed game seen the same way.
class Archive {
public:
    struct Entry {
        std::string name;
        i64 offset;
        i64 size;
    };

    virtual ~Archive() = default;
    virtual SourcePtr open(const Entry& entry) = 0;

    const std::vector<Entry>& entries() const { return _entries; }
    const std::string& path() const { return _path; }
    i64 length() const { return _length; }
    const Entry* find(const std::string& name) const;

protected:
    std::vector<Entry> _entries;
    std::string _path;
    i64 _length = 0;
};

using ArchivePtr = std::shared_ptr<Archive>;

/// An NSP or NSZ, which are PFS0 archives, or an XCI or XCZ, whose content is in the secure partition of a cartridge image.
/// The kind is worked out from the file itself. The file is never written to.
class FileArchive : public Archive {
public:
    explicit FileArchive(const std::string& path);
    SourcePtr open(const Entry& entry) override;

    static bool isGameFile(const std::string& path);

private:
    void readPartition();
    void readCartridge(i64 header);
    std::vector<Entry> readHfs0(i64 start, bool mustFit);

    SourcePtr _file;
};

struct TitleItem {
    ArchivePtr archive;
    ContentMeta meta;
    /// Where the content meta is, in the file.
    std::string metaName;
    /// What it's made with. If the content meta had to be worked out, that's the program instead.
    NcaPtr metaNca;
};

using ItemPtr = std::shared_ptr<TitleItem>;

struct SectionSource {
    NcaFile* owner;
    const NcaFsSection* section;
    NcaFile* original;
};

/// One content of a title: an update's NCA takes over from the base game's, but may still read from it.
struct ContentPair {
    u8 type;
    u8 idOffset;
    NcaPtr base;
    NcaPtr patch;

    NcaFile& primary() const { return patch ? *patch : *base; }
    bool sectionAt(int index, SectionSource& out) const;
    /// Section numbers in the order the sections lie in the file. Going through them like this means a solid NCZ gets read from
    /// front to back only once.
    std::vector<int> sectionsInFileOrder() const;
    void releaseMemory() const;
};

struct TitleJob {
    ItemPtr main;
    ItemPtr update;
    std::vector<ContentPair> contents;
    i64 inputBytes = 0;
    std::string titleName;

    bool isBaseGame() const { return main->meta.type == Cnmt::MetaApplication; }
    void releaseMemory() const;
    std::string displayName() const;
    std::string detail() const;
    std::string describe() const;
    std::string stem(u32 version, bool sharedFile) const;
};

struct InputIssue {
    std::string path;
    std::string message;
};

/// What a game file says about a title in it, which only takes a few small reads.
struct TitleSummary {
    u64 titleId = 0;
    u8 type = 0;
    u32 version = 0;
    u64 linkedId = 0;
    i64 bytes = 0;
    /// A base game that's been cut down to what its update still needs.
    bool holdsOnlyPart = false;
    std::string name;
    /// The version people know it by, such as 1.0.2.
    std::string displayVersion;
    Bytes icon;
};

/// Opens game files and works out which titles they hold and which update goes with which base game.
class TitleSet {
public:
    explicit TitleSet(const Keyset& keyset);

    TicketStore& tickets() { return _tickets; }
    void add(const ArchivePtr& archive);
    /// Which update goes with which base game. Two files of one title come to one job, the newest, unless every file is wanted: then each
    /// is a job of its own, and the update goes with the newest copy it's newer than.
    void group(bool everyFile = false);
    /// What's in the files added so far, without going into any more of them than that takes.
    std::vector<TitleSummary> summarize();

    std::vector<TitleJob> jobs;
    std::vector<InputIssue> problems;
    std::vector<std::string> notes;

    bool sharesFile(const TitleJob& job) const;

    /// Turns files and folders into a list of game files, searching folders and subfolders. What's in the folder of an extracted game
    /// is that game, whatever else it may look like, so those aren't gone into if asked. What this tool has already compressed is left out,
    /// as it would otherwise count as a second copy of a title, unless it's asked for.
    static std::vector<std::string> expand(const std::vector<std::string>& inputs, std::vector<std::string>& notes, std::vector<InputIssue>& problems, const std::vector<std::string>& foldersToSkip = {}, bool skipExtracted = false, bool ownOutputToo = false);

private:
    ItemPtr newestAndNoteOthers(std::vector<ItemPtr> items, const char* kind);
    bool holdsOnlyPart(const TitleItem& item);
    bool updateGoesWith(const TitleItem& update, const TitleItem& base);
    void addJob(const ItemPtr& main, const ItemPtr& update);
    i64 inputBytesOf(const ItemPtr& main, const ItemPtr& update) const;
    NcaPtr openContent(const TitleItem& item, const ContentMetaRecord& record);
    ItemPtr itemFromContents(const ArchivePtr& archive, const Archive::Entry& metaEntry);
    std::string readTitleName(const std::vector<ContentPair>& contents);

    const Keyset& _keyset;
    TicketStore _tickets;
    std::vector<ArchivePtr> _archives;
    std::vector<ItemPtr> _items;
};

struct ConvertOptions {
    bool compress = true;
    int level = 9;
    int blockSize = 0x10000;
    int threads = 0;
    bool noSign = false;
    /// Where what's been read of a solid NCZ waits if it has to be gone back to.
    std::string tempDir;
    /// How much of an NCA to keep in memory while it's written, as what's kept doesn't have to be read back to work out its hash.
    i64 holdInMemory = 0;
    /// A small section that has to be gone through more than once goes through memory, up to this size, rather than being read each time.
    i64 keepSmallSections = 0;
    /// Titles that would save less than this percentage aren't worth the time. Zero means none are left out.
    int minSavingPercent = 0;
    /// Game data that's already stored compressed goes across as it is, rather than being compressed all over again.
    bool keepCompressed = false;
    /// Read back what's written and compare it. What it comes to is the same either way.
    bool checkResult = true;
    /// It's known not to be on a hard drive, so a slow read isn't one that's had to go looking.
    bool noSeekTime = false;
};

/// Where a rebuilt title goes: an NSP, or the console's own storage.
class TitleOutput {
public:
    virtual ~TitleOutput() = default;
    /// One stream per NCA, all finished before the next begins. The size is negative if it isn't known yet.
    virtual NcaStream& beginNca(const char* label, i64 size, const char* extension) = 0;
    virtual void endNca(const NcaInfo& info, const char* extension) = 0;
    /// The whole title is out, and this is what describes it.
    virtual void finish(const ContentMeta& rebuilt, const NcaInfo& metaNca) = 0;
};

/// Writes an NSP in one pass: a reserved PFS0 header, then each NCA straight into the file, then the real header. NCA names have
/// a fixed length, so the header size is known before their hashes are. The file only gets its final name once it is complete.
class NspOutput : public TitleOutput, private NcaStream {
public:
    /// The size it may come to says what kind of file it takes, on a card that can't hold 4 GB in one, and how much room to set aside.
    NspOutput(std::string path, int contentCount, i64 sizeHint = 0);
    ~NspOutput() override;

    static i64 headerSize(int contentCount);

    NcaStream& beginNca(const char* label, i64 size, const char* extension) override;
    void endNca(const NcaInfo& info, const char* extension) override;
    void finish(const ContentMeta& rebuilt, const NcaInfo& metaNca) override;

    const std::string& path() const { return _path; }
    i64 outputBytes() const { return _position; }

private:
    void write(const void* data, size_t size) override;
    void overwrite(i64 offset, const void* data, size_t size) override;
    void readBack(i64 offset, void* data, size_t size) override;

    struct FileRecord {
        std::string name;
        i64 offset;
        i64 size;
    };

    std::string _path;
    std::string _partPath;
    sys::File _file;
    i64 _dataStart;
    int _expectedFiles;
    i64 _position;
    i64 _ncaStart = 0;
    std::vector<FileRecord> _files;
    bool _reserved = false;
    bool _finished = false;
};

/// A folder holding a game's parts, the way extracting leaves them, and the way hacBrewPack takes them: exefs, romfs, logo, control,
/// htmldoc and legalinfo.
struct ExtractedGame {
    std::string folder;
    std::string name;
    u64 titleId = 0;
    u32 version = 0;
    i64 inputBytes = 0;
    bool hasRomFs = false;
    bool hasLogo = false;
    bool hasHtmlDoc = false;
    bool hasLegalInfo = false;
    /// All the files of its game data, gone through once, as there can be hundreds of thousands.
    SourcePtr romFsImage;

    static bool isOne(const std::string& folder);
    /// Throws if something it needs isn't there, or isn't right.
    static ExtractedGame open(const std::string& folder);

    int contentCount() const;
    std::string outputFileName(const ConvertOptions& options) const;
    /// The JPEG its icon is stored as, which doesn't take opening all of it. Empty if it has none.
    static Bytes readIcon(const std::string& folder);
};

/// A folder as a RomFS image, or the files directly in it as a PFS0, without copying anything. Replacements take the place of files of
/// that name at the top of the folder.
SourcePtr buildRomFsImage(const std::string& folder, const std::map<std::string, Bytes>& replacements = {});
SourcePtr buildRomFsImageOf(const std::map<std::string, Bytes>& files);
SourcePtr buildPfs0FromFolder(const std::string& folder, const std::map<std::string, Bytes>& replacements = {});

/// Rebuilds a title as a new Application or DLC with no ticket needed. Program and Data RomFS sections are stored compressed, an
/// update is applied onto its base, and everything else is decrypted and re-encrypted byte for byte.
class TitleConverter {
public:
    TitleConverter(ConvertOptions options, const Keyset& keyset);

    void run(const TitleJob& job, TitleOutput& output);
    /// Makes an Application out of an extracted game.
    void build(const ExtractedGame& game, TitleOutput& output);

    /// A small Data NCA made of this image, compressed like any other, to see whether a console can read it.
    NcaInfo writeProbe(TitleOutput& output, const SourcePtr& image, u64 programId, const char* label);

    enum class Plan { CompressRomFs, RebuildRomFs, RebuildPfs0, PassThrough };
    static Plan plan(const ContentPair& content, const SectionSource& source, const ConvertOptions& options);

private:
    NcaInfo writeContent(TitleOutput& output, const ContentPair& content, bool& compressed);
    NcaSection buildSection(const ContentPair& content, const SectionSource& source, bool& sign);
    void compressInto(NcaBuilder& builder, int index, const SourcePtr& image, const char* label);
    NcaInfo writeFolderNca(TitleOutput& output, const char* label, NcaContentType type, const std::string& folder, u64 titleId, const std::map<std::string, Bytes>& replacements);
    NcaSection passThrough(const SectionSource& source);

    ConvertOptions _options;
    std::unique_ptr<NcaWriter> _writer;
};

/// The output name a rebuilt title gets: Title [TitleID][vVersion][LZ4].nsp
std::string outputFileName(const TitleJob& job, const TitleSet& set, const ConvertOptions& options);

/// The JPEG a title's icon is stored as. Empty if it has none. Only something to look at, so it needn't take the time to be checked.
Bytes readTitleIcon(const TitleJob& job, bool verify = true);

/// Writes a title out as the same folders the build mode takes: exefs, romfs, logo, control, htmldoc and legalinfo. With an update,
/// the files are the updated game's.
struct ExtractResult {
    std::string path;
    i64 bytes = 0;
    i64 files = 0;
};
ExtractResult extractTitle(const TitleJob& job, const TitleSet& set, const std::string& parent);
/// The folder that would go in.
std::string extractedFolderOf(const TitleJob& job, const TitleSet& set, const std::string& parent);

/// All its game data is already stored compressed, or there's none compressing would touch, with no update to make part of it, so going
/// through it again wouldn't come to anything.
bool isAlreadyCompressed(const TitleJob& job, const ConvertOptions& options);

/// Reads all of a title through its hash trees, and throws if any of it isn't what it should be.
void checkTitle(const TitleJob& job, const std::string& tempDir);

struct SizeEstimate {
    i64 inputBytes = 0;
    i64 outputBytes = 0;
    i64 marginBytes = 0;
    i64 romFsBytes = 0;
    i64 romFsStoredBytes = 0;
    /// All of its game data is already stored the way this would store it, as opposed to it just not compressing well.
    bool alreadyCompressed = false;

    i64 savedBytes() const { return inputBytes - outputBytes; }
    double savedFraction() const { return inputBytes > 0 ? (double)savedBytes() / (double)inputBytes : 0; }
};

/// Predicts a conversion without doing it. Everything but the compressed data is worked out exactly from the headers; for the
/// compressed data, a spread of regions from across the RomFS is really compressed, with the same encoder, level and block size
/// the conversion would use. Hashes aren't checked, so only the sampled regions are read.
SizeEstimate estimateTitle(const TitleJob& job, const ConvertOptions& options);
SizeEstimate estimateBuild(const ExtractedGame& game, const ConvertOptions& options);

/// Whether a title is worth the time it takes to compress, for people after the most space in the least time.
namespace SavingRule {
bool applies(const ConvertOptions& options);
bool worthCompressing(const SizeEstimate& estimate, const ConvertOptions& options);
std::string reason(const SizeEstimate& estimate, const ConvertOptions& options);
}  // namespace SavingRule

}  // namespace hbp
