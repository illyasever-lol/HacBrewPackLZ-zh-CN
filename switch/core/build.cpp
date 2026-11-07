// Building a title from folders, the way hacBrewPack does. Everything is described without copying anything, and the layout and
// ordering follow hacBrewPack, whose output is known to work on hardware.
#include <algorithm>
#include <cstdio>

#include "title.h"

namespace hbp {

namespace {

/// One file open at a time, whichever was last read from: a game can be made of more files than can be open at once.
struct OpenFile {
    sys::Mutex mutex;
    std::string path;
    std::unique_ptr<sys::File> file;
};

class FolderFile : public DataSource {
public:
    FolderFile(std::shared_ptr<OpenFile> open, std::string path, i64 size) : _open(std::move(open)), _path(std::move(path)), _size(size) {}

    i64 length() const override { return _size; }

    void read(i64 offset, void* buffer, size_t count) override {
        // There's nothing to keep open for a small file, which can then be read at the same time as any other.
        constexpr i64 Small = 1 << 20;
        if (_size < Small) {
            sys::File file(_path, sys::File::Mode::Read);
            file.read(offset, buffer, count);
            return;
        }
        sys::Lock lock(_open->mutex);
        if (_open->path != _path) {
            _open->file.reset(new sys::File(_path, sys::File::Mode::Read));
            _open->path = _path;
        }
        _open->file->read(offset, buffer, count);
    }

    bool readsAtOnce() const override { return true; }

    void release() override {
        sys::Lock lock(_open->mutex);
        _open->file.reset();
        _open->path.clear();
    }

private:
    std::shared_ptr<OpenFile> _open;
    std::string _path;
    i64 _size;
};

Bytes readWhole(const std::string& path) {
    sys::File file(path, sys::File::Mode::Read);
    Bytes data((size_t)file.size());
    if (!data.empty())
        file.read(0, data.data(), data.size());
    return data;
}

struct DirNode;

struct FileNode {
    std::string name;
    std::string sortKey;
    std::string path;
    i64 size = 0;
    DirNode* parent = nullptr;
    const Bytes* replacement = nullptr;
    FileNode* sibling = nullptr;
    u32 entryOffset = 0;
    i64 offset = 0;
};

struct DirNode {
    std::string name;
    std::string sortKey;
    std::string path;
    DirNode* parent = nullptr;
    DirNode* sibling = nullptr;
    DirNode* firstChild = nullptr;
    FileNode* firstFile = nullptr;
    u32 entryOffset = 0;
};

constexpr u32 EmptySlot = 0xFFFFFFFF;
constexpr i64 FilePartitionOffset = 0x200;

i64 alignTo(i64 value, i64 alignment) { return (value + alignment - 1) & ~(alignment - 1); }

u32 pathHash(u32 parent, const std::string& name) {
    u32 hash = parent ^ 123456789;
    for (char c : name) {
        hash = hash >> 5 | hash << 27;
        hash ^= (u8)c;
    }
    return hash;
}

std::vector<u32> newHashTable(size_t entries) {
    u32 count;
    if (entries < 3) {
        count = 3;
    } else if (entries < 19) {
        count = (u32)entries | 1;
    } else {
        count = (u32)entries;
        while (count % 2 == 0 || count % 3 == 0 || count % 5 == 0 || count % 7 == 0 || count % 11 == 0 || count % 13 == 0 || count % 17 == 0)
            count++;
    }
    return std::vector<u32>(count, EmptySlot);
}

Bytes toBytes(const std::vector<u32>& values) {
    Bytes bytes(values.size() * 4);
    for (size_t i = 0; i < values.size(); i++)
        wr32(bytes.data() + i * 4, values[i]);
    return bytes;
}

/// Sorted as bytes, which is how the names end up ordered in the image.
bool byKey(const std::string& a, const std::string& b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) { return (u8)x < (u8)y; });
}

struct Tree {
    std::vector<std::unique_ptr<DirNode>> ownedDirs;
    std::vector<std::unique_ptr<FileNode>> ownedFiles;
    std::vector<DirNode*> dirs;
    std::vector<FileNode*> files;
};

void addEntries(DirNode* dir, const std::vector<sys::DirEntry>& entries, Tree& tree, const std::map<std::string, Bytes>& replacements, std::vector<DirNode*>& next) {
    std::vector<DirNode*> childDirs;
    std::vector<FileNode*> childFiles;
    for (auto& entry : entries) {
        std::string key = dir->sortKey + '\\' + entry.name;
        std::string path = dir->path + "/" + entry.name;
        if (entry.isDirectory) {
            tree.ownedDirs.emplace_back(new DirNode);
            DirNode* child = tree.ownedDirs.back().get();
            child->name = entry.name;
            child->sortKey = key;
            child->path = path;
            child->parent = dir;
            childDirs.push_back(child);
        } else {
            tree.ownedFiles.emplace_back(new FileNode);
            FileNode* child = tree.ownedFiles.back().get();
            child->name = entry.name;
            child->sortKey = key;
            child->path = path;
            child->size = entry.size;
            child->parent = dir;
            if (!dir->parent) {
                auto replaced = replacements.find(entry.name);
                if (replaced != replacements.end()) {
                    child->replacement = &replaced->second;
                    child->size = (i64)replaced->second.size();
                }
            }
            childFiles.push_back(child);
        }
    }

    std::sort(childDirs.begin(), childDirs.end(), [](DirNode* a, DirNode* b) { return byKey(a->sortKey, b->sortKey); });
    std::sort(childFiles.begin(), childFiles.end(), [](FileNode* a, FileNode* b) { return byKey(a->sortKey, b->sortKey); });
    for (size_t i = 0; i + 1 < childDirs.size(); i++)
        childDirs[i]->sibling = childDirs[i + 1];
    for (size_t i = 0; i + 1 < childFiles.size(); i++)
        childFiles[i]->sibling = childFiles[i + 1];
    dir->firstChild = childDirs.empty() ? nullptr : childDirs[0];
    dir->firstFile = childFiles.empty() ? nullptr : childFiles[0];

    tree.dirs.insert(tree.dirs.end(), childDirs.begin(), childDirs.end());
    tree.files.insert(tree.files.end(), childFiles.begin(), childFiles.end());
    next.insert(next.end(), childDirs.begin(), childDirs.end());
}

/// Most of the time this takes is waiting for the card, which a few folders at a time make better use of.
void visit(DirNode* root, Tree& tree, const std::map<std::string, Bytes>& replacements) {
    std::vector<DirNode*> level{root};
    while (!level.empty()) {
        std::vector<std::vector<sys::DirEntry>> listed(level.size());
        parallelFor(sys::FileThreads, (int)level.size(), [&](int index, int) {
            checkStop();
            listed[(size_t)index] = sys::list(level[(size_t)index]->path);
        });
        std::vector<DirNode*> next;
        for (size_t i = 0; i < level.size(); i++)
            addEntries(level[i], listed[i], tree, replacements, next);
        sink().counting("正在浏览其文件", (i64)tree.files.size());
        level.swap(next);
    }
}

i64 folderBytes(const std::string& folder) {
    i64 total = 0;
    for (auto& entry : sys::list(folder))
        total += entry.isDirectory ? folderBytes(folder + "/" + entry.name) : entry.size;
    return total;
}

}  // namespace

static SourcePtr imageOfTree(Tree& tree, DirNode& root);

SourcePtr buildRomFsImage(const std::string& folder, const std::map<std::string, Bytes>& replacements) {
    Tree tree;
    DirNode root;
    root.path = folder;
    visit(&root, tree, replacements);
    return imageOfTree(tree, root);
}

SourcePtr buildRomFsImageOf(const std::map<std::string, Bytes>& files) {
    Tree tree;
    DirNode root;
    FileNode* last = nullptr;
    for (auto& file : files) {
        tree.ownedFiles.emplace_back(new FileNode);
        FileNode* node = tree.ownedFiles.back().get();
        node->name = file.first;
        node->sortKey = "\\" + file.first;
        node->size = (i64)file.second.size();
        node->parent = &root;
        node->replacement = &file.second;
        if (last)
            last->sibling = node;
        else
            root.firstFile = node;
        last = node;
        tree.files.push_back(node);
    }
    return imageOfTree(tree, root);
}

static SourcePtr imageOfTree(Tree& tree, DirNode& root) {
    std::sort(tree.dirs.begin(), tree.dirs.end(), [](DirNode* a, DirNode* b) { return byKey(a->sortKey, b->sortKey); });
    std::sort(tree.files.begin(), tree.files.end(), [](FileNode* a, FileNode* b) { return byKey(a->sortKey, b->sortKey); });
    tree.dirs.insert(tree.dirs.begin(), &root);

    u32 entryOffset = 0;
    i64 partitionSize = 0;
    for (FileNode* file : tree.files) {
        partitionSize = alignTo(partitionSize, 0x10);
        file->offset = partitionSize;
        partitionSize += file->size;
        file->entryOffset = entryOffset;
        entryOffset += 0x20u + (u32)alignTo((i64)file->name.size(), 4);
    }
    u32 fileTableSize = entryOffset;

    entryOffset = 0;
    for (DirNode* dir : tree.dirs) {
        dir->entryOffset = entryOffset;
        entryOffset += 0x18u + (u32)alignTo((i64)dir->name.size(), 4);
    }
    u32 dirTableSize = entryOffset;

    std::vector<u32> dirHash = newHashTable(tree.dirs.size());
    std::vector<u32> fileHash = newHashTable(tree.files.size());
    Bytes dirTable(dirTableSize), fileTable(fileTableSize);

    for (FileNode* file : tree.files) {
        u8* e = fileTable.data() + file->entryOffset;
        u32 bucket = pathHash(file->parent->entryOffset, file->name) % (u32)fileHash.size();
        wr32(e, file->parent->entryOffset);
        wr32(e + 0x04, file->sibling ? file->sibling->entryOffset : EmptySlot);
        wr64(e + 0x08, (u64)file->offset);
        wr64(e + 0x10, (u64)file->size);
        wr32(e + 0x18, fileHash[bucket]);
        wr32(e + 0x1C, (u32)file->name.size());
        if (!file->name.empty())
            memcpy(e + 0x20, file->name.data(), file->name.size());
        fileHash[bucket] = file->entryOffset;
    }

    for (DirNode* dir : tree.dirs) {
        u8* e = dirTable.data() + dir->entryOffset;
        u32 parent = dir->parent ? dir->parent->entryOffset : 0;
        u32 bucket = pathHash(parent, dir->name) % (u32)dirHash.size();
        wr32(e, parent);
        wr32(e + 0x04, dir->sibling ? dir->sibling->entryOffset : EmptySlot);
        wr32(e + 0x08, dir->firstChild ? dir->firstChild->entryOffset : EmptySlot);
        wr32(e + 0x0C, dir->firstFile ? dir->firstFile->entryOffset : EmptySlot);
        wr32(e + 0x10, dirHash[bucket]);
        wr32(e + 0x14, (u32)dir->name.size());
        if (!dir->name.empty())
            memcpy(e + 0x18, dir->name.data(), dir->name.size());
        dirHash[bucket] = dir->entryOffset;
    }

    i64 dirHashOffset = alignTo(partitionSize + FilePartitionOffset, 4);
    i64 dirHashSize = (i64)dirHash.size() * 4;
    i64 dirTableOffset = dirHashOffset + dirHashSize;
    i64 fileHashOffset = dirTableOffset + dirTableSize;
    i64 fileHashSize = (i64)fileHash.size() * 4;
    i64 fileTableOffset = fileHashOffset + fileHashSize;

    Bytes header((size_t)FilePartitionOffset);
    const i64 fields[] = {0x50, dirHashOffset, dirHashSize, dirTableOffset, dirTableSize, fileHashOffset, fileHashSize, fileTableOffset, fileTableSize, FilePartitionOffset};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
        wr64(header.data() + i * 8, (u64)fields[i]);

    auto open = std::make_shared<OpenFile>();
    auto image = std::make_shared<ConcatSource>();
    image->add(bytesSource(std::move(header)));
    i64 position = 0;
    for (FileNode* file : tree.files) {
        image->add(std::make_shared<ZeroSource>(file->offset - position));
        if (file->replacement)
            image->add(bytesSource(*file->replacement));
        else
            image->add(std::make_shared<FolderFile>(open, file->path, file->size));
        position = file->offset + file->size;
    }
    image->add(std::make_shared<ZeroSource>(dirHashOffset - FilePartitionOffset - position));
    image->add(bytesSource(toBytes(dirHash)));
    image->add(bytesSource(std::move(dirTable)));
    image->add(bytesSource(toBytes(fileHash)));
    image->add(bytesSource(std::move(fileTable)));
    return image;
}

SourcePtr buildPfs0FromFolder(const std::string& folder, const std::map<std::string, Bytes>& replacements) {
    std::vector<sys::DirEntry> entries;
    for (auto& entry : sys::list(folder)) {
        if (!entry.isDirectory)
            entries.push_back(entry);
    }
    std::sort(entries.begin(), entries.end(), [](const sys::DirEntry& a, const sys::DirEntry& b) {
        return std::lexicographical_compare(a.name.begin(), a.name.end(), b.name.begin(), b.name.end(), [](char x, char y) { return toupper((u8)x) < toupper((u8)y); });
    });

    auto open = std::make_shared<OpenFile>();
    std::vector<std::pair<std::string, SourcePtr>> files;
    for (auto& entry : entries) {
        auto replaced = replacements.find(entry.name);
        if (replaced != replacements.end())
            files.emplace_back(entry.name, bytesSource(replaced->second));
        else
            files.emplace_back(entry.name, std::make_shared<FolderFile>(open, folder + "/" + entry.name, entry.size));
    }
    return buildPfs0(files);
}

bool ExtractedGame::isOne(const std::string& folder) { return sys::exists(folder + "/exefs/main.npdm") && sys::exists(folder + "/control/control.nacp"); }

ExtractedGame ExtractedGame::open(const std::string& folder) {
    ExtractedGame game;
    game.folder = folder;

    std::string npdmPath = folder + "/exefs/main.npdm";
    if (!sys::exists(npdmPath))
        throw ToolError("'" + npdmPath + "' 未找到，ExeFS 需要 main.npdm");
    Bytes npdm = readWhole(npdmPath);
    if (npdm.size() < 0x80 || memcmp(npdm.data(), "META", 4) != 0)
        throw ToolError("'" + npdmPath + "' 不是有效的 NPDM（META 魔数错误）");
    int aci0 = rdi32(&npdm[0x70]);
    if (aci0 < 0 || (size_t)aci0 + 0x18 > npdm.size() || memcmp(&npdm[(size_t)aci0], "ACI0", 4) != 0)
        throw ToolError("'" + npdmPath + "' 不是有效的 NPDM（ACI0 区段错误）");
    game.titleId = rd64(&npdm[(size_t)aci0 + 0x10]);
    if (game.titleId < 0x0100000000000000ull || game.titleId > 0x0FFFFFFFFFFFFFFFull)
        throw ToolError(format("Invalid title ID %016llx in '%s'", (unsigned long long)game.titleId, npdmPath.c_str()));

    std::string nacpPath = folder + "/control/control.nacp";
    if (!sys::exists(nacpPath))
        throw ToolError("'" + nacpPath + "' 未找到，control 文件夹需要 control.nacp");
    Bytes nacp = readWhole(nacpPath);
    if (nacp.size() < Nacp::Size)
        throw ToolError("'" + nacpPath + "' 太小，不是有效的 NACP");
    auto titles = Nacp::titles(nacp.data(), nacp.size());
    game.name = Nacp::choose(titles).name;
    bool hasPublisher = std::any_of(titles.begin(), titles.end(), [](const NacpTitle& title) { return !title.publisher.empty(); });
    if (game.name.empty())
        throw ToolError("' 中没有游戏名称" + nacpPath + "'");
    if (!hasPublisher)
        throw ToolError("' 中没有发行商" + nacpPath + "'");

    game.hasRomFs = sys::isDirectory(folder + "/romfs");
    if (game.hasRomFs) {
        Tree tree;
        DirNode root;
        root.path = folder + "/romfs";
        visit(&root, tree, {});
        for (FileNode* file : tree.files)
            game.inputBytes += file->size;
        game.romFsImage = imageOfTree(tree, root);
    }
    game.hasLogo = sys::isDirectory(folder + "/logo");
    game.hasHtmlDoc = sys::isDirectory(folder + "/htmldoc");
    game.hasLegalInfo = sys::isDirectory(folder + "/legalinfo");

    // Extracting leaves the version in the folder's name, and a game had better not come back older than it was.
    size_t slash = folder.find_last_of("/\\");
    std::string last = slash == std::string::npos ? folder : folder.substr(slash + 1);
    size_t tag = last.rfind("[v");
    unsigned version = 0;
    if (tag != std::string::npos && sscanf(last.c_str() + tag, "[v%u]", &version) == 1)
        game.version = version;

    for (const char* part : {"exefs", "logo", "control", "htmldoc", "legalinfo"})
        game.inputBytes += folderBytes(folder + "/" + part);
    return game;
}

int ExtractedGame::contentCount() const { return 2 + (hasHtmlDoc ? 1 : 0) + (hasLegalInfo ? 1 : 0); }

std::string ExtractedGame::outputFileName(const ConvertOptions& options) const {
    return format("%s [%016llX][v%u]%s.nsp", LocalNames::forOutputName(name).c_str(), (unsigned long long)titleId, version, options.compress ? "[LZ4]" : "");
}

Bytes ExtractedGame::readIcon(const std::string& folder) {
    constexpr i64 MaxIconSize = 1 << 20;
    std::string best;
    for (auto& entry : sys::list(folder + "/control")) {
        if (entry.isDirectory || entry.size <= 0 || entry.size > MaxIconSize || entry.name.rfind("icon_", 0) != 0 || !endsWithNoCase(entry.name, ".dat"))
            continue;
        if (best.empty() || entry.name == "icon_AmericanEnglish.dat")
            best = entry.name;
    }
    if (best.empty())
        return {};
    try {
        return readWhole(folder + "/control/" + best);
    } catch (const ToolError&) {
        return {};
    }
}

}  // namespace hbp
