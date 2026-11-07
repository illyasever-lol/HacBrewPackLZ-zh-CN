// A pretend console, kept in a folder, that behaves as the real services do where it matters: contents are files named after their
// IDs, metas and records are small files that change all at once, and placeholders stay behind if the power goes. It can also be told
// to lose power at its Nth call, which is how the replacement of a game gets tested at every step.
#pragma once

#include <atomic>

#include <map>

#include "../core/console.h"

namespace hbp {

/// Thrown where the power goes. Nothing that runs after this counts, as the real thing wouldn't get that far.
struct PowerCut {};

class SimConsole : public ConsoleApi {
public:
    SimConsole(std::string folder, const Keyset& keyset);

    /// Starts from a list of NSPs, as if they'd been installed.
    void install(const std::vector<std::string>& nspPaths, Storage storage);

    /// The Nth call from now on loses power. Zero means it never does.
    void losePowerAt(int call) {
        _powerCutAt = call;
        _calls = 0;
        _powerLost = false;
    }
    /// The power has gone, so nothing tidies up after itself.
    bool powerLost() const { return _powerLost; }
    int calls() const { return _calls; }
    /// The firmware refuses to mount anything, as without sigpatches.
    void setFirmwareRefuses(bool refuses) { _firmwareRefuses = refuses; }
    /// Reads of written contents come back with a wrong byte.
    void setCardCorrupts(bool corrupts) { _cardCorrupts = corrupts; }
    void setFree(Storage storage, i64 bytes) { _free[storage == Storage::Sd ? 1 : 0] = bytes; }
    /// What the home menu would show for a game.
    void setAppearance(u64 applicationId, std::string name, Bytes icon) { _appearance[applicationId] = {std::move(name), std::move(icon)}; }

    /// Every game on the home menu has all it needs. Returns what's wrong, if anything.
    std::string checkConsistent(const Keyset& keyset);
    /// Contents and placeholders nothing refers to.
    std::vector<std::string> strayFiles();

    std::vector<u64> listApplications() override;
    InstalledGame describe(u64 applicationId) override;
    void readControl(InstalledGame& game) override;
    SourcePtr openContent(Storage storage, const ContentId& id, i64 size) override;
    void addTitleKeys(TicketStore& store, const Keyset& keyset) override;
    i64 freeBytes(Storage storage) override;
    i64 totalBytes(Storage storage) override;
    std::unique_ptr<ContentWriter> createContent(Storage storage, i64 size) override;
    void deletePlaceholder(Storage storage, const std::string& token) override;
    bool hasContent(Storage storage, const ContentId& id) override;
    void deleteContent(Storage storage, const ContentId& id) override;
    void setLaunchVersion(u64 applicationId, u32 version) override { launchVersions[applicationId] = version; }
    std::map<u64, u32> launchVersions;
    void setMeta(Storage storage, const MetaKey& key, const Bytes& installMeta) override;
    bool hasMeta(Storage storage, const MetaKey& key) override;
    void removeMeta(Storage storage, const MetaKey& key) override;
    void setRecords(u64 applicationId, const std::vector<std::pair<MetaKey, Storage>>& records) override;
    std::vector<std::pair<MetaKey, Storage>> records(u64 applicationId) override;
    std::unique_ptr<FirmwareView> mountGameData(Storage storage, const ContentId& id, u64 programId) override;
    std::unique_ptr<FirmwareView> mountLogo(Storage storage, const ContentId& id, u64 programId) override;

    /// Called by what it hands out, too.
    void tick();

    /// How often names and icons have been read, which is what takes the time on the real thing.
    std::atomic<int> controlReads{0};

    std::string contentPath(Storage storage, const ContentId& id) const;
    std::string placeholderPath(Storage storage, const std::string& token) const;
    bool cardCorrupts() const { return _cardCorrupts; }
    i64 freeFor(Storage storage) const { return _free[storage == Storage::Sd ? 1 : 0]; }

private:
    std::string storageFolder(Storage storage) const;
    std::string metaPath(Storage storage, const MetaKey& key) const;
    std::string recordsPath(u64 applicationId) const;

    std::string _folder;
    const Keyset& _keyset;
    std::atomic<int> _calls{0};
    int _powerCutAt = 0;
    bool _powerLost = false;
    bool _firmwareRefuses = false;
    bool _cardCorrupts = false;
    i64 _free[2] = {-1, -1};
    int _nextPlaceholder = 1;
    std::vector<Bytes> _tickets;
    std::map<u64, std::pair<std::string, Bytes>> _appearance;
};

}  // namespace hbp
