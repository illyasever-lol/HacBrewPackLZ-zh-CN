// Game files and titles: what a file or an installed game holds, which update goes with which base game, and what it takes to
// rebuild a title, estimate it or extract it.
#pragma once

#include <algorithm>
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

/// In a content meta's attributes: a base game cut down to what its update reads from it, which only works with that update.
constexpr u8 AttributeCompacted = 0x04;

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
    /// Its game data is compressed the way this tool compresses it, which its headers say, whatever the file is called.
    bool madeHere = false;
    /// What the game came to before, as a copy this tool made says. Zero if it doesn't say, as a copy from before 1.5 doesn't.
    i64 originalBytes = 0;
    /// The largest of its game data packed as one solid stream, as an NSZ's solid compression packs it, which an update can only read out
    /// of order by keeping it aside. Zero if none is.
    i64 solidBytes = 0;
};

/// Where an update kept each part of the original game as it was. A game compressed together with its update holds those parts where
/// that update put them, so with this, a newer update, which reads the same parts of the original, can be made part of it later without
/// the original. It's kept as a small file in the game's control data, beside its name and icons, where nothing else looks.
struct UpdateMap {
    static constexpr const char* FileName = "hacbrewpacklz_update_map.txt";
    u32 version = 0;
    std::vector<KeptSection> sections;

    /// What a game made from this job keeps of the original. False if there's nothing to keep track of. Only what's kept as it was in
    /// the game data counts: an update compressed by its makers keeps some of the original inside what it compresses. A copy made again,
    /// compressed, keeps the map it was made with.
    static bool forJob(const TitleJob& job, bool compress, UpdateMap& out);
    /// The map a copy was made with, in its control data, if it has one for this version.
    static bool inControl(const std::vector<ContentPair>& contents, u32 version, UpdateMap& out);
    Bytes toBytes() const;
    /// False for anything that isn't a map this made.
    static bool parse(const Bytes& data, UpdateMap& out);
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
    /// For a copy this made, with an update in it already, what that update kept of the original, which a newer one reads: from the
    /// update itself if it's here, or else from the map the copy was made with.
    void keepOriginal(std::vector<ContentPair>& contents, const ItemPtr& main, const ItemPtr& update);
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

/// 12.0.0, the first system version that reads compressed game data, as a game's record writes a version.
constexpr u32 CompressionMinSystemVersion = 12u << 26;

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
    /// A game's new file holds its DLC too, after it in title ID order. Off, each DLC gets a file of its own.
    bool dlcInGameFile = true;
    /// Game data that's already stored compressed goes across as it is, rather than being compressed all over again.
    bool keepCompressed = false;
    /// Read back what's written and compare it. What it comes to is the same either way.
    bool checkResult = true;
    /// It's known not to be on a hard drive, so a slow read isn't one that's had to go looking.
    bool noSeekTime = false;
    /// A copy an older version made is looked at like any other game, as made again it can come out smaller: what's installed on the
    /// console, where the space is what counts. So is one its developer compressed, which made again here can come out smaller too, though
    /// its updates then need its original.
    bool improveOlderCopies = false;
    /// Measuring, every this many of the blocks stored where they are is compressed, and what those come to goes for the rest. Every one of
    /// them, 1, makes the figure exact, which is for games under MeasureExactlyBelow, where it takes little longer and the margin would be
    /// the most of what they save.
    int measureEvery = RomFsCompressor::MeasureEvery;
    /// The most a copy made in place of an installed game asks of the system: what the console's own record of the game says, which an installer
    /// such as DBI can have lowered so a console on an older system version starts it. The game's files still say what it first asked for,
    /// and a copy made from them would ask for that again, so the console would say it needs a system update. Compressing still needs 12.0.0.
    /// Left at its most for anything else.
    u32 consoleRequiredSystem = UINT32_MAX;
};

/// A game smaller than this, as the list shows it, is measured exactly: every block compressed, in about twice the time. Above it, the
/// margin of measuring every 16th block is a small part of what a game saves. The same on the PC (Analyzer.MeasureExactlyBelow).
constexpr i64 MeasureExactlyBelow = 1LL << 30;

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
    /// The size it may come to says what kind of file it takes, on a card that can't hold 4 GB in one, and how much room to set aside. A file
    /// already at the path is only replaced when that's asked for, as by a tool given a path to write to: the app saves under a free name.
    /// Several titles can go into one file, a game and then its DLC, each written by TitleConverter::run with this same output. The file is
    /// only finished after the last of them. The content count covers all of them, not counting their meta NCAs.
    NspOutput(std::string path, int contentCount, i64 sizeHint = 0, bool replace = false, int titleCount = 1);
    ~NspOutput() override;

    static i64 headerSize(int contentCount, int titleCount = 1);

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
    int _titlesLeft;
    i64 _position;
    i64 _ncaStart = 0;
    std::vector<FileRecord> _files;
    /// Room is set aside a step at a time as the file reaches it, rather than all of it at once, and what's written gets flushed as it goes:
    /// if the console goes down part way through, the card has as little as possible left half done.
    void makeRoomFor(i64 end);
    bool _setsAside = false;
    i64 _setAside = 0;
    i64 _unflushed = 0;
    bool _replace = false;
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

    /// What extracting puts in a game's folder first and takes away once all of it is out, the same as on the PC. While it's there, the folder
    /// is only part of a game, even with the exefs and control folders out already, which make it look like one: it isn't listed or built.
    /// A power cut part of the way through leaves it there.
    static constexpr const char* NotFinished = "Not finished extracting.txt";
    static constexpr const char* NotFinishedText = "hacBrewPackLZ was extracting a game into this folder and didn't finish. It isn't listed as a game. "
                                                   "Extracting the game again starts afresh, or delete this folder.";

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
/// The same, of files in memory, with '/' between folders. Folders with nothing in them can be given too.
SourcePtr buildRomFsImageOf(const std::map<std::string, Bytes>& files, const std::vector<std::string>& folders = {});
SourcePtr buildPfs0FromFolder(const std::string& folder, const std::map<std::string, Bytes>& replacements = {});

/// The version written into what's made, which the PC writes too, as its own version. tools/set_version.py sets it with the rest.
constexpr const char* EngineVersion = "1.5.1";

/// What a copy this tool made says about itself, kept as a small file in its control data beside its name and icons, where nothing else
/// looks: which version made it, so a later one knows which copies it can make smaller still, and what the game came to before, so what
/// compressing it freed can be shown for as long as it's there. As CopyMark on the PC, byte for byte.
struct CopyMark {
    static constexpr const char* FileName = "hacbrewpacklz_copy.txt";
    /// The first version that stores what a game holds more than once only once, wherever it is.
    static constexpr const char* StoresRepeatsOnce = "1.5.0";
    std::string madeBy;
    i64 originalBytes = 0;

    bool isCurrent() const;
    Bytes toBytes() const;
    /// False for anything that isn't a mark this made.
    static bool parse(const Bytes& data, CopyMark& out);
    /// The mark in a game's control data. False if it has none.
    static bool in(const TitleJob& job, CopyMark& out);
    /// What a copy made from this job says: this version, and what the game came to before this tool made anything of it.
    static CopyMark forJob(const TitleJob& job, const ConvertOptions& options);
    /// What the game takes installed as it is, from its content meta, whatever it's packed in, with the update made part of it.
    static i64 installedBytes(const TitleJob& job);
    /// Versions by their numbers, 1.10 after 1.9, and a test version as the one it leads to.
    static int compareVersions(const std::string& a, const std::string& b);
};

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

    /// Game data found held again while compressing, rather than through an update, in everything this has made. No estimate knows of it,
    /// so what's made can come to up to that much less than its estimate.
    i64 foundBytes() const { return _foundBytes; }

private:
    NcaInfo writeContent(TitleOutput& output, const ContentPair& content, bool& compressed);
    NcaSection buildSection(const ContentPair& content, const SectionSource& source, bool& sign);
    void compressInto(NcaBuilder& builder, int index, const SourcePtr& image, const char* label, const std::vector<KeptRange>* kept = nullptr);
    NcaInfo writeFolderNca(TitleOutput& output, const char* label, NcaContentType type, const std::string& folder, u64 titleId, const std::map<std::string, Bytes>& replacements);
    NcaSection passThrough(const SectionSource& source);

    /// The control data with what this tool keeps there added: the map of what's kept of the original, and what the copy says about itself.
    /// False if it isn't laid out as expected.
    bool controlWithNotes(const SectionSource& source, NcaSection& out);

    ConvertOptions _options;
    std::unique_ptr<NcaWriter> _writer;
    /// What the game being made keeps of the original, written into its control data.
    UpdateMap _map;
    bool _hasMap = false;
    /// What the copy being made says about itself, written into its control data too.
    CopyMark _mark;
    bool _hasMark = false;
    i64 _foundBytes = 0;
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


/// Whether an update reads true over what it's given as its base game, by reading some of the game data it makes, spread across it, with the
/// hash checks on. A compressed copy with an earlier update in it doesn't hold all of the original that a newer one reads, nor does a copy of a
/// game its makers compressed, and that shows up here, before any time goes on compressing it. Anything that slips through is still caught by
/// the same checks when it's compressed.
bool updateFits(const TitleJob& job);

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
    /// For a copy this tool made, what compressing it freed: what the game came to before, which the copy says, less what it is now.
    i64 freedBytes = 0;
    /// Its makers compressed it, rather than this tool.
    bool byMakers = false;
    /// A copy an older version made, which made again can come out smaller, and is looked at like any other game.
    bool olderCopy = false;
    /// For a game file packed with Zstandard, as an NSZ is: the file itself, and what it takes installed as it is, which every other installer
    /// unpacks it to. Installed, it's compared with the second; made into an NSP, with the first. Zero for anything else.
    i64 packedBytes = 0, installedBytes = 0;

    i64 savedBytes() const { return inputBytes - outputBytes; }
    double savedFraction() const { return inputBytes > 0 ? (double)savedBytes() / (double)inputBytes : 0; }

    /// What it's sure to save: the estimate less its margin. What a sample can't see, such as data that level packs share, only ever makes it
    /// save more, so this is what's shown, as "at least", and what the minimum saving goes by. One that would grow says by how much, as it is.
    i64 sureSavedBytes() const { return savedBytes() < 0 ? savedBytes() : std::max<i64>(0, savedBytes() - marginBytes); }
    double sureSavedFraction() const { return inputBytes > 0 ? (double)sureSavedBytes() / (double)inputBytes : 0; }
    /// What it comes to after, to go with what it's sure to save, so the two always add up.
    i64 mostBytes() const { return inputBytes - sureSavedBytes(); }
    /// Whether what it saves is only known to be at least that, rather than worked out to the byte.
    bool atLeast() const { return marginBytes > 0 && savedBytes() >= 0; }

    /// Growing by more than a sliver as a fact about it, which is shown as growing, rather than a first look's say. Compressing stores what it
    /// can't shrink as it is, and games a first look put over their size came to a few kilobytes less when compressed on a console. It's a
    /// fact once all of it has been read, when nothing is compressed (a repack unpacks what's packed), and for an NSZ made into a file and
    /// compared with that file, which is packed tighter than LZ4 packs.
    bool sureToGrow(bool compressing) const {
        return savedFraction() < -GrowsShare && (measured || !compressing || (packedBytes > 0 && inputBytes == packedBytes));
    }
    static constexpr double GrowsShare = 0.01;

    /// Worked out from reading all of the game, rather than from a sample of it, so everything stored once is in it.
    bool measured = false;
};

/// Whether this tool compressed its game data, rather than its makers.
bool isMadeByThisTool(const TitleJob& job);
bool hasCompressedGameData(const TitleJob& job);
/// What a game's contents come to unpacked, from what they say, as a content meta counts them.
i64 unpackedBytes(const TitleJob& job, const ConvertOptions& options);
/// What compressing a copy this tool made freed.
i64 freedBy(const TitleJob& job, const ConvertOptions& options);
/// A game whose game data is all compressed already: nothing to gain, and for a copy this tool made, what compressing it freed.
SizeEstimate alreadyCompressedEstimate(const TitleJob& job, const ConvertOptions& options);

/// Predicts a conversion without doing it. Everything but the compressed data is worked out exactly from the headers; for the
/// compressed data, a spread of regions from across the RomFS is really compressed, with the same encoder, level and block size
/// the conversion would use. Hashes aren't checked, so only the sampled regions are read.
SizeEstimate estimateTitle(const TitleJob& job, const ConvertOptions& options);
/// What compressing comes to, from reading all of the game once and writing nothing, with its hashes checked as it's read, so a damaged game
/// fails here rather than part way through being written. Everything compressing stores once is in it, and how well the rest compresses is
/// measured on a fixed share of it, the same every time. For a game a first look can't vouch for. As Analyzer.Measure on the PC.
SizeEstimate measureTitle(const TitleJob& job, const ConvertOptions& options);
/// Measured, its game data is read in full, as measureTitle does.
SizeEstimate estimateBuild(const ExtractedGame& game, const ConvertOptions& options, bool measure = false);

/// Whether a title is worth the time it takes to compress, for people after the most space in the least time.
namespace SavingRule {
bool applies(const ConvertOptions& options);
bool worthCompressing(const SizeEstimate& estimate, const ConvertOptions& options);
/// A first look can say a game is worth compressing, but not that it isn't: what a sample can't see, such as data that level packs share,
/// only makes a game save more. So one it can't vouch for isn't left out on its say: it's measured first, by reading all of it and writing
/// nothing, and compressed only if what's measured is worth it. Only what's known about the file itself, that it's already compressed, leaves
/// a game out without that. As SavingRule.NotSure on the PC.
bool notSure(const SizeEstimate& estimate, const ConvertOptions& options);
std::string reason(const SizeEstimate& estimate, const ConvertOptions& options);
}  // namespace SavingRule

}  // namespace hbp
