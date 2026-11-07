// Runs the whole app on a PC. A pretend person presses the buttons, the screen is saved as pictures to look at, and what became of the
// pretend console is checked afterwards.
//   app_test -k keys folder shots demo
//   app_test -k keys folder shots real base.nsp update.nsp [other.nsp]
//
// With UPDATE_DIR set to a folder holding what tools\sign_release.py made for a program.bin of version 9.9.9, updating gets tried too.
#include <winsock2.h>
#include <windows.h>
#include <psapi.h>
#include <wincodec.h>

#pragma comment(lib, "psapi.lib")

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "../app/app.h"
#include "../core/remote.h"
#include "sim_console.h"

using namespace hbp;

namespace {

int g_failures = 0;
int g_passes = 0;

void report(const std::string& name, bool ok, const std::string& detail = "") {
    if (ok) {
        g_passes++;
        printf("PASS  %s\n", name.c_str());
    } else {
        g_failures++;
        printf("FAIL  %s  %s\n", name.c_str(), detail.c_str());
    }
    fflush(stdout);
}

// -------------------------------------------------------------------------------------------------------------------
// Pictures

u32 crc32Of(const u8* data, size_t size, u32 crc = 0) {
    static u32 table[256];
    if (!table[1]) {
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    crc = ~crc;
    for (size_t i = 0; i < size; i++)
        crc = table[(crc ^ data[i]) & 255] ^ (crc >> 8);
    return ~crc;
}

void writePng(const std::string& path, const u32* pixels, int width, int height) {
    Bytes raw;
    raw.reserve((size_t)height * (width * 3 + 1));
    for (int y = 0; y < height; y++) {
        raw.push_back(0);
        for (int x = 0; x < width; x++) {
            u32 p = pixels[(size_t)y * width + x];
            raw.push_back((u8)p);
            raw.push_back((u8)(p >> 8));
            raw.push_back((u8)(p >> 16));
        }
    }

    Bytes zlib = {0x78, 0x01};
    u32 a = 1, b = 0;
    for (u8 value : raw) {
        a = (a + value) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t position = 0; position < raw.size();) {
        size_t n = std::min<size_t>(65535, raw.size() - position);
        zlib.push_back(position + n == raw.size() ? 1 : 0);
        zlib.push_back((u8)n);
        zlib.push_back((u8)(n >> 8));
        zlib.push_back((u8)~n);
        zlib.push_back((u8)(~n >> 8));
        zlib.insert(zlib.end(), raw.begin() + position, raw.begin() + position + n);
        position += n;
    }
    u32 adler = b << 16 | a;
    for (int i = 3; i >= 0; i--)
        zlib.push_back((u8)(adler >> (i * 8)));

    FILE* file = fopen(path.c_str(), "wb");
    if (!file)
        throw ToolError("Can't write " + path);
    const u8 signature[] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(signature, 1, 8, file);
    auto chunk = [file](const char* type, const Bytes& data) {
        u8 length[4] = {(u8)(data.size() >> 24), (u8)(data.size() >> 16), (u8)(data.size() >> 8), (u8)data.size()};
        fwrite(length, 1, 4, file);
        Bytes all(type, type + 4);
        all.insert(all.end(), data.begin(), data.end());
        fwrite(all.data(), 1, all.size(), file);
        u32 crc = crc32Of(all.data(), all.size());
        u8 end[4] = {(u8)(crc >> 24), (u8)(crc >> 16), (u8)(crc >> 8), (u8)crc};
        fwrite(end, 1, 4, file);
    };
    Bytes header = {(u8)(width >> 24), (u8)(width >> 16), (u8)(width >> 8), (u8)width, (u8)(height >> 24), (u8)(height >> 16), (u8)(height >> 8), (u8)height, 8, 2, 0, 0, 0};
    chunk("IHDR", header);
    chunk("IDAT", zlib);
    chunk("IEND", {});
    fclose(file);
}

bool decodeJpegWithWindows(const Bytes& jpeg, Image& out) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return false;
    bool ok = false;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapSource* converted = nullptr;
    if (SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromMemory((BYTE*)jpeg.data(), (DWORD)jpeg.size())) && SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) && SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame, &converted))) {
        UINT w = 0, h = 0;
        converted->GetSize(&w, &h);
        out.w = (int)w;
        out.h = (int)h;
        out.pixels.resize((size_t)w * h);
        ok = SUCCEEDED(converted->CopyPixels(nullptr, w * 4, w * h * 4, (BYTE*)out.pixels.data()));
    }
    if (converted)
        converted->Release();
    if (frame)
        frame->Release();
    if (decoder)
        decoder->Release();
    if (stream)
        stream->Release();
    factory->Release();
    return ok;
}

// -------------------------------------------------------------------------------------------------------------------
// The pretend system

Bytes readWhole(const std::string& path) {
    sys::File file(path, sys::File::Mode::Read);
    Bytes data((size_t)file.size());
    if (!data.empty())
        file.read(0, data.data(), data.size());
    return data;
}

std::string readText(const std::string& path) {
    if (!sys::exists(path))
        return "";
    Bytes data = readWhole(path);
    return std::string(data.begin(), data.end());
}

void writeText(const std::string& path, const std::string& text) {
    sys::File file(path, sys::File::Mode::Create);
    file.write(0, text.data(), text.size());
}

class HostPlatform : public Platform {
public:
    HostPlatform(std::string folder, std::string keys, ConsoleApi* console) : _folder(std::move(folder)), _keys(std::move(keys)), _console(console) {}

    ConsoleApi* console() override { return _console; }
    std::string consoleProblem() override { return "The service that lists games wouldn't start (2001-0123)."; }

    void loadFonts(Font& font) override {
        static Bytes regular = readWhole("C:/Windows/Fonts/segoeui.ttf");
        font.addFace(regular.data(), regular.size());
    }

    bool decodeIcon(const Bytes& jpeg, Image& out) override {
        if (jpeg.size() == 4) {
            // A made-up icon for the made-up games: a gradient in the colour given.
            out.w = out.h = 256;
            out.pixels.resize(256 * 256);
            for (int y = 0; y < 256; y++) {
                for (int x = 0; x < 256; x++) {
                    double light = 0.55 + 0.45 * (x + y) / 510.0;
                    bool shape = (x - 128) * (x - 128) + (y - 128) * (y - 128) < 70 * 70;
                    u8 r = (u8)(jpeg[0] * light), g = (u8)(jpeg[1] * light), b = (u8)(jpeg[2] * light);
                    if (shape) {
                        r = (u8)(255 - (255 - r) / 3);
                        g = (u8)(255 - (255 - g) / 3);
                        b = (u8)(255 - (255 - b) / 3);
                    }
                    out.pixels[(size_t)y * 256 + x] = 0xFF000000u | b << 16 | g << 8 | r;
                }
            }
            return true;
        }
        return decodeJpegWithWindows(jpeg, out);
    }

    bool prefersDark() override { return dark; }
    std::string dataFolder() override { return _folder + "/data"; }
    std::string backupFolder() override { return _folder + "/config"; }
    std::string outputFolder() override { return _folder + "/output"; }
    std::vector<Drive> drives() override {
        std::vector<Drive> list{{"SD card", _folder + "/sd", false}};
        if (usb)
            list.push_back(Drive{"USB: Test Drive (exFAT)", _folder + "/usb", true});
        return list;
    }
    bool drivesChanged() override { return usbChanged.exchange(false); }
    std::vector<std::string> foldersToSkip() override { return {_folder + "/sd/Nintendo"}; }
    std::vector<std::string> keyLocations() override { return {_keys}; }
    void setWorking(bool isWorking, bool isBoosted) override {
        working = isWorking;
        boosted = isBoosted;
        if (isWorking)
            workedAtAll = true;
    }
    int batteryPercent() override { return battery; }
    bool isCharging() override { return false; }
    bool memoryIsLimited() override { return limited; }
    bool firmwareReadsCompression() override { return newFirmware; }
    std::string aboutSystem() override { return "A PC"; }
    bool isOnline() override { return online; }
    Bytes download(const std::string& url, size_t limit, const std::function<bool()>&) override {
        auto found = web.find(url);
        if (found == web.end())
            throw ToolError("Nothing at " + url);
        if (found->second.size() > limit)
            throw ToolError("Too much at " + url);
        return found->second;
    }
    std::string programPath() override { return program; }
    std::string releaseFileName() override { return "program.bin"; }
    bool restartAfterExit(const std::string& path) override {
        restartedAs = path;
        return true;
    }

    bool online = false;
    std::map<std::string, Bytes> web;
    std::string program;
    std::string restartedAs;
    bool usb = false;
    std::atomic<bool> usbChanged{false};
    bool dark = true;
    bool working = false;
    bool boosted = false;
    bool workedAtAll = false;
    int battery = 80;
    bool limited = false;
    bool newFirmware = true;

private:
    std::string _folder;
    std::string _keys;
    ConsoleApi* _console;
};

/// Plenty of made-up games with made-up answers, for looking at how the screens fill up.
class DemoConsole : public ConsoleApi {
public:
    struct Game {
        const char* name;
        const char* version;
        double gigabytes;
        int dlc;
        double ratio;
        bool icon;
        const char* blocked;
    };

    static const std::vector<Game>& games() {
        static const std::vector<Game> list = {
            {"The Legend of the Hollow Crown: Tears of Time and Space, Definitive Collector's Edition", "1.2.1", 16.2, 2, 0.41, true, ""},
            {"Stardew Acres", "1.6.8", 0.48, 0, 0.32, true, ""},
            {"Metro Exodus Redux", "1.0.2", 27.4, 3, 0.18, true, ""},
            {"Hollow Songs", "1.5.78", 6.9, 0, 0.27, true, ""},
            {"Super Kart Racing Deluxe", "3.0.1", 13.8, 2, 0.06, false, ""},
            {"Celestial Peak", "1.4.0", 0.96, 0, 0.12, true, ""},
            {"Portal Knights Remastered", "2.1.0", 5.3, 1, 0.44, true, ""},
            {"Dark Chronicles IV", "1.0.5", 31.2, 0, 0.02, false, ""},
            {"A Short Stroll", "1.3.0", 0.3, 0, 0.14, true, ""},
            {"Tiny Tunes Adventure", "1.1.0", 2.2, 0, 0.51, true, ""},
            {"Fire Saga: Three Banners", "1.8.0", 14.9, 4, 0.22, true, "It's running at the moment. Open this app through a different game to compress this one"},
            {"Animal Gardens", "2.0.0", 11.1, 1, 0.0, true, ""},
            {"Rogue Cells", "3.3.0", 1.1, 3, 0.09, false, ""},
            {"Neon Drift Racing", "2.4.1", 4.6, 0, -0.04, true, ""},
        };
        return list;
    }

    static u64 idOf(size_t index) { return 0x0100000000010000ull + ((u64)index << 16); }

    static std::string fingerprintOf(const InstalledGame& game) {
        std::vector<std::string> ids;
        for (auto& meta : game.metas) {
            for (auto& content : meta.contents)
                ids.push_back(content.id.hex());
        }
        std::sort(ids.begin(), ids.end());
        std::string all;
        for (auto& id : ids)
            all += id;
        u8 hash[32];
        sha256(all.data(), all.size(), hash);
        return toHex(hash, 8);
    }

    /// Answers as if every game had already been analyzed.
    static void writeAnalysis(const std::string& path, DemoConsole& console) {
        std::string text;
        auto& list = games();
        for (size_t i = 0; i < list.size(); i++) {
            InstalledGame game = console.describe(idOf(i));
            i64 now = game.totalBytes();
            i64 after = (i64)(now * (1 - list[i].ratio));
            text += format("4|%016llx|%s|9|65536\t%lld %lld %lld %lld %lld 0\n", (unsigned long long)idOf(i), fingerprintOf(game).c_str(), (long long)now, (long long)after, (long long)(now / 100), (long long)now, (long long)after);
        }
        writeText(path, text);
    }

    std::vector<u64> listApplications() override {
        std::vector<u64> ids;
        for (size_t i = 0; i < games().size(); i++)
            ids.push_back(idOf(i));
        return ids;
    }

    InstalledGame describe(u64 applicationId) override {
        size_t index = (size_t)((applicationId - 0x0100000000010000ull) >> 16);
        const Game& source = games().at(index);
        InstalledGame game;
        game.applicationId = applicationId;
        game.blocked = source.blocked;
        InstalledMeta base;
        base.key = MetaKey{applicationId, 0, Cnmt::MetaApplication, 0};
        base.storage = index % 5 == 3 ? Storage::Nand : Storage::Sd;
        InstalledContent content;
        content.id.bytes[0] = (u8)index;
        content.size = (i64)(source.gigabytes * (1 << 30));
        base.contents.push_back(content);
        game.metas.push_back(base);
        if (index % 2 == 0) {
            InstalledMeta patch;
            patch.key = MetaKey{applicationId + 0x800, 65536, Cnmt::MetaPatch, 0};
            patch.storage = base.storage;
            game.metas.push_back(patch);
        }
        for (int i = 0; i < source.dlc; i++) {
            InstalledMeta dlc;
            dlc.key = MetaKey{applicationId + 0x1000 + i + 1, 0, Cnmt::MetaAddOnContent, 0};
            dlc.storage = base.storage;
            game.metas.push_back(dlc);
        }
        return game;
    }

    void readControl(InstalledGame& game) override {
        size_t index = (size_t)((game.applicationId - 0x0100000000010000ull) >> 16);
        const Game& source = games().at(index);
        game.name = source.name;
        game.displayVersion = source.version;
        if (source.icon)
            game.icon = {(u8)(60 + index * 37 % 170), (u8)(70 + index * 73 % 150), (u8)(90 + index * 113 % 150), 255};
    }

    SourcePtr openContent(Storage, const ContentId&, i64) override { throw ToolError("This is only a picture of a console"); }
    void addTitleKeys(TicketStore&, const Keyset&) override {}
    i64 freeBytes(Storage storage) override { return storage == Storage::Sd ? 38LL << 30 : 4LL << 30; }
    i64 totalBytes(Storage storage) override { return storage == Storage::Sd ? 238LL << 30 : 26LL << 30; }
    std::unique_ptr<ContentWriter> createContent(Storage, i64) override { throw ToolError("This is only a picture of a console"); }
    void deletePlaceholder(Storage, const std::string&) override {}
    bool hasContent(Storage, const ContentId&) override { return false; }
    void deleteContent(Storage, const ContentId&) override {}
    void setMeta(Storage, const MetaKey&, const Bytes&) override {}
    bool hasMeta(Storage, const MetaKey&) override { return false; }
    void removeMeta(Storage, const MetaKey&) override {}
    void setRecords(u64, const std::vector<std::pair<MetaKey, Storage>>&) override {}
    std::vector<std::pair<MetaKey, Storage>> records(u64) override { return {}; }
    void setLaunchVersion(u64, u32) override {}
    std::unique_ptr<FirmwareView> mountGameData(Storage, const ContentId&, u64) override { throw ToolError("This is only a picture of a console"); }
    std::unique_ptr<FirmwareView> mountLogo(Storage, const ContentId&, u64) override { throw ToolError("This is only a picture of a console"); }
};

// -------------------------------------------------------------------------------------------------------------------
// The pretend person

class Person {
public:
    Person(App& app, std::string shots) : _app(app), _shots(std::move(shots)), _pixels(1280 * 720) {}

    /// The welcome screen only gets looked at where that's what's being tried.
    bool readsWelcome = false;

    void frame(const InputState& input = InputState()) {
        InputState given = input;
        if (!readsWelcome && _app.showingWelcome())
            given.held |= ButtonPlus;
        _app.update(given, 1 / 60.0);
        if (_app.needsDrawing()) {
            Canvas canvas(_pixels.data(), 1280, 720, 1280);
            _app.draw(canvas);
        }
    }

    void settle(int frames = 45) {
        for (int i = 0; i < frames; i++)
            frame();
    }

    void press(u32 button) {
        InputState down;
        down.held = button;
        frame(down);
        frame();
        settle(20);
    }

    void waitUntilIdle(double seconds = 600) {
        // One thing can lead to another: only a while with nothing going on counts.
        double start = sys::now();
        int quiet = 0;
        while (quiet < 20 && sys::now() - start < seconds) {
            frame();
            quiet = _app.busy() ? 0 : quiet + 1;
            sys::sleepMs(5);
        }
        settle();
    }

    /// True once the condition holds. It's looked at between frames.
    template <typename Condition>
    bool waitFor(Condition condition, double seconds = 600) {
        double start = sys::now();
        while (sys::now() - start < seconds) {
            frame();
            if (condition())
                return true;
            sys::sleepMs(10);
        }
        return false;
    }

    void shot(const std::string& name) {
        Canvas canvas(_pixels.data(), 1280, 720, 1280);
        _app.draw(canvas);
        writePng(_shots + "/" + name + ".png", _pixels.data(), 1280, 720);
    }

private:
    App& _app;
    std::string _shots;
    std::vector<u32> _pixels;
};

const App::RowView* rowNamed(const App::Snapshot& snapshot, const std::string& part) {
    for (auto& row : snapshot.rows) {
        if (containsNoCase(row.name, part))
            return &row;
    }
    return nullptr;
}

constexpr int SettingsTab = 2;

/// The settings most of these go by, whatever a new copy of the app would start with.
void startOver(const std::string& root) {
    sys::removeFile(root + "/config/settings.txt");
    sys::makeDirectories(root + "/data");
    writeText(root + "/data/settings.txt", "welcomed=1\ninstallgamefiles=0\nminimumsaving=0\ncheckwritten=1\nhidecompressed=0\nmoresettings=1\nwritelog=1\nfrompcnetwork=0\nfrompccable=0\n");
}

/// So that a list has to be gone through from nothing, which takes long enough to see what else is going on meanwhile.
void forgetIndex(const std::string& root) {
    sys::removeFile(root + "/data/files.cache");
    sys::removeFile(root + "/config/files.cache");
}

void wipeData(const std::string& root) {
    sys::removeTree(root + "/data");
    sys::removeTree(root + "/config");
    startOver(root);
}

/// L and R, the short way round.
void goToTab(Person& person, App& app, int tab) {
    int from = app.snapshot().tab;
    int forward = (tab - from + 3) % 3;
    for (int i = 0; i < std::min(forward, 3 - forward); i++)
        person.press(forward <= 1 ? ButtonR : ButtonL);
}

/// A PC that takes the connection and is never heard from again, as one that stops answering would be. Each connection is kept until
/// it closes.
class SilentPc {
public:
    explicit SilentPc(u16 port) {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
        _listening = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in here = {};
        here.sin_family = AF_INET;
        here.sin_port = htons(port);
        here.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(_listening, (sockaddr*)&here, sizeof(here));
        listen(_listening, 8);
        _thread.start([this] {
            while (true) {
                SOCKET taken = accept(_listening, nullptr, nullptr);
                if (taken == INVALID_SOCKET)
                    return;
                sys::Lock lock(_mutex);
                _taken.push_back(taken);
            }
        }, -1, 1 << 16, false);
    }
    ~SilentPc() {
        closesocket(_listening);
        _thread.join();
        for (SOCKET taken : _taken)
            closesocket(taken);
    }

    int connections() {
        sys::Lock lock(_mutex);
        return (int)_taken.size();
    }

private:
    SOCKET _listening;
    sys::Thread _thread;
    sys::Mutex _mutex;
    std::vector<SOCKET> _taken;
};

/// Up and down to an option on the settings tab, wherever it is in the list.
std::string optionValue(App& app, const std::string& label);

void goToOption(Person& person, App& app, const std::string& label) {
    goToTab(person, app, SettingsTab);
    App::Snapshot snapshot = app.snapshot();
    for (int i = 0; i < (int)snapshot.options.size(); i++) {
        if (snapshot.options[(size_t)i].rfind(label + ":", 0) != 0)
            continue;
        while (app.snapshot().optionFocus != i)
            person.press(app.snapshot().optionFocus < i ? ButtonDown : ButtonUp);
        return;
    }
    // Most settings are only there once More settings has been chosen.
    if (label != "More settings" && optionValue(app, "More settings") != "(no such option)") {
        goToOption(person, app, "More settings");
        person.press(ButtonA);
        person.settle();
        if (optionValue(app, "More settings") == "(no such option)" || app.snapshot().options.size() > snapshot.options.size())
            return goToOption(person, app, label);
    }
    throw ToolError("There's no option called " + label);
}

std::string optionValue(App& app, const std::string& label) {
    for (auto& option : app.snapshot().options) {
        if (option.rfind(label + ": ", 0) == 0)
            return option.substr(label.size() + 2);
    }
    return "(no such option)";
}

/// Opens the + menu and leaves it open, on the row that has that in it.
bool goToMenuRow(Person& person, App& app, const std::string& part) {
    person.press(ButtonPlus);
    auto goTo = [&](int i) {
        while (app.snapshot().menuFocus != i)
            person.press(app.snapshot().menuFocus < i ? ButtonDown : ButtonUp);
    };
    auto find = [&]() {
        std::vector<std::string> rows = app.snapshot().menu;
        for (int i = 0; i < (int)rows.size(); i++) {
            if (containsNoCase(rows[(size_t)i], part))
                return i;
        }
        return -1;
    };
    int found = find();
    if (found >= 0) {
        goTo(found);
        return true;
    }
    // On one of the pages.
    std::vector<std::string> first = app.snapshot().menu;
    for (int i = 0; i < (int)first.size(); i++) {
        if (first[(size_t)i].size() < 4 || first[(size_t)i].compare(first[(size_t)i].size() - 4, 4, " ...") != 0)
            continue;
        goTo(i);
        person.press(ButtonA);
        found = find();
        if (found >= 0) {
            goTo(found);
            return true;
        }
        person.press(ButtonB);
    }
    person.press(ButtonB);
    return false;
}

bool chooseInMenu(Person& person, App& app, const std::string& part) {
    if (!goToMenuRow(person, app, part)) {
        report("the menu has " + part, false);
        return false;
    }
    person.press(ButtonA);
    return true;
}

int runDemo(const std::string& keys, const std::string& folder, const std::string& shots) {
    for (bool dark : {true, false}) {
        std::string data = folder + (dark ? "/demo_dark" : "/demo_light");
        sys::removeTree(data);
        sys::makeDirectories(data + "/data");
        sys::makeDirectories(data + "/sd");
        DemoConsole console;
        DemoConsole::writeAnalysis(data + "/data/analysis.txt", console);
        HostPlatform platform(data, keys, &console);
        platform.dark = dark;
        App app(platform);
        app.start();
        Person person(app, shots);
        std::string theme = dark ? "dark" : "light";
        person.readsWelcome = true;
        person.settle();
        person.shot(theme + "_welcome");
        report("the first time, the welcome screen comes up", app.snapshot().screen == "Welcome", app.snapshot().screen);
        person.press(ButtonDown);
        person.press(ButtonDown);
        person.shot(theme + "_welcome_choice");
        person.readsWelcome = false;
        person.press(ButtonPlus);
        person.waitUntilIdle();
        person.shot(theme + "_list_top");

        for (int i = 0; i < 9; i++)
            person.press(ButtonDown);
        person.shot(theme + "_list_scrolled");
        // The end of the list, where a game that would come out larger goes, and back.
        for (int i = 0; i < 5; i++)
            person.press(ButtonDown);
        person.shot(theme + "_list_end");
        for (int i = 0; i < 5; i++)
            person.press(ButtonUp);

        person.press(ButtonA);
        goToTab(person, app, SettingsTab);
        person.shot(theme + "_options");
        person.press(ButtonRight);
        person.press(ButtonRight);
        person.press(ButtonRight);
        person.shot(theme + "_options_changed");
        for (int i = 0; i < 15; i++)
            person.press(ButtonDown);
        person.shot(theme + "_options_end");
        for (int i = 0; i < 15; i++)
            person.press(ButtonUp);
        goToTab(person, app, 0);
        person.settle();
        person.shot(theme + "_list_with_minimum");

        App::Snapshot snapshot = app.snapshot();
        report("the rule leaves out what saves too little", rowNamed(snapshot, "Dark Chronicles") && !rowNamed(snapshot, "Dark Chronicles")->ticked);
        report("the rule ticks what saves enough", rowNamed(snapshot, "Tiny Tunes") && rowNamed(snapshot, "Tiny Tunes")->ticked);

        person.press(ButtonPlus);
        person.shot(theme + "_menu");
        person.press(ButtonRight);
        person.shot(theme + "_sorted_by_size");
        person.press(ButtonB);
        person.press(ButtonY);
        person.shot(theme + "_ticks_changed");
        person.settle(200);

        person.press(ButtonX);
        person.shot(theme + "_compress_dialog");
        person.press(ButtonB);

        person.press(ButtonR);
        person.waitUntilIdle();
        person.shot(theme + "_files_empty");
        person.press(ButtonPlus);
        person.shot(theme + "_menu_files");
        person.press(ButtonB);
        goToMenuRow(person, app, "Only look in one folder");
        person.press(ButtonA);
        person.shot(theme + "_browse");
        person.press(ButtonA);
        person.shot(theme + "_browse_inside");
        person.press(ButtonMinus);
        person.press(ButtonB);
        person.shot(theme + "_quit");
        person.press(ButtonB);
    }

    // No keys yet.
    {
        std::string data = folder + "/demo_nokeys";
        sys::removeTree(data);
        sys::makeDirectories(data);
        DemoConsole console;
        HostPlatform platform(data, data + "/prod.keys", &console);
        App app(platform);
        app.start();
        Person person(app, shots);
        person.settle();
        person.shot("nokeys");
        report("no keys, and it says so", app.snapshot().screen == "NoKeys");
    }

    // The console's services can't be had.
    {
        std::string data = folder + "/demo_noconsole";
        sys::removeTree(data);
        sys::makeDirectories(data + "/sd");
        HostPlatform platform(data, keys, nullptr);
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        person.shot("noconsole_files");
        person.press(ButtonL);
        person.shot("noconsole_installed");
    }
    printf("%d passed, %d failed\n", g_passes, g_failures);
    return g_failures > 0 ? 1 : 0;
}

/// A look at what text comes to with fonts of every kind of outline. The console's own fonts can't be had here.
int runFonts(const std::string& shots) {
    std::vector<u32> pixels(1280 * 720);
    Canvas canvas(pixels.data(), 1280, 720, 1280);
    canvas.clear(Color{27, 29, 32});
    static const char* const paths[] = {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/Crimson-Roman.otf", "C:/Windows/Fonts/NotoSansJP-Regular.otf"};
    float y = 40;
    bool allRead = true;
    for (const char* path : paths) {
        Bytes data = readWhole(path);
        Font font;
        font.addFace(data.data(), data.size());
        allRead = allRead && font.hasFaces();
        Color text{238, 240, 242};
        canvas.text(font, 40, y, path, 15, Color{163, 170, 178});
        y += 34;
        canvas.text(font, 40, y, "The Legend of the Hollow Crown: Tears of Time 0123456789", 27, text);
        y += 30;
        canvas.text(font, 40, y, "Version 1.2.1 with its update  \xC2\xB7  2 DLC  \xC2\xB7  On the SD card  (2.20 GB, 51%)", 15, text);
        y += 30;
        canvas.text(font, 40, y, "Compressing the game data", 20, text, true);
        y += 34;
        canvas.text(font, 40, y, "\xE3\x82\xBC\xE3\x83\xAB\xE3\x83\x80\xE3\x81\xAE\xE4\xBC\x9D\xE8\xAA\xAC \xE3\x81\x82\xE3\x81\x84\xE3\x81\x86 abc", 24, text);
        y += 72;
    }
    report("fonts of every kind can be read", allRead);
    writePng(shots + "/fonts.png", pixels.data(), 1280, 720);
    return g_failures > 0 ? 1 : 0;
}

/// Draws the icon the app shows on the home screen of the homebrew menu, and saves it as a JPEG.
int writeIcon(const std::string& path) {
    constexpr int Size = 256;
    std::vector<u32> pixels(Size * Size);
    Canvas c(pixels.data(), Size, Size, Size);
    c.clear(Color{27, 29, 32});
    const Color teal{45, 190, 172};
    const Color pale{226, 244, 241};

    // A tall stack of data, squeezed down to a short one.
    int width[] = {150, 150, 150};
    for (int i = 0; i < 3; i++)
        c.fillRound(Rect{(Size - width[i]) / 2, 44 + i * 24, width[i], 16}, 5, pale.withAlpha(90 + i * 30));
    c.line(128, 120, 128, 150, 9, teal);
    c.line(128, 158, 106, 134, 9, teal);
    c.line(128, 158, 150, 134, 9, teal);
    c.fillRound(Rect{53, 172, 150, 30}, 9, teal);

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return 1;
    std::wstring wide(path.begin(), path.end());
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* properties = nullptr;
    bool ok = SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(wide.c_str(), GENERIC_WRITE)) && SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) && SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, &properties));
    if (ok) {
        PROPBAG2 option = {};
        wchar_t name[] = L"ImageQuality";
        option.pstrName = name;
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = 0.95f;
        properties->Write(1, &option, &value);
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        Bytes bgr((size_t)Size * Size * 3);
        for (int i = 0; i < Size * Size; i++) {
            u32 p = pixels[(size_t)i];
            bgr[(size_t)i * 3] = (u8)(p >> 16);
            bgr[(size_t)i * 3 + 1] = (u8)(p >> 8);
            bgr[(size_t)i * 3 + 2] = (u8)p;
        }
        ok = SUCCEEDED(frame->Initialize(properties)) && SUCCEEDED(frame->SetSize(Size, Size)) && SUCCEEDED(frame->SetPixelFormat(&format)) && SUCCEEDED(frame->WritePixels(Size, Size * 3, (UINT)bgr.size(), bgr.data())) && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }
    if (properties)
        properties->Release();
    if (frame)
        frame->Release();
    if (encoder)
        encoder->Release();
    if (stream)
        stream->Release();
    factory->Release();
    printf(ok ? "wrote %s\n" : "couldn't write %s\n", path.c_str());
    return ok ? 0 : 1;
}

/// How much memory the hash tree of a very large game takes, which only has to be there once. A game of 160 GB comes to a good 300 MB of it.
int runHashTree(int gigabytes) {
    i64 bytes = (i64)gigabytes << 30;
    Bytes data(1 << 20);
    PROCESS_MEMORY_COUNTERS before = {};
    GetProcessMemoryInfo(GetCurrentProcess(), &before, sizeof(before));
    IvfcBuilder builder;
    builder.expect(bytes);
    for (i64 done = 0; done < bytes; done += (i64)data.size())
        builder.add(data.data(), data.size());
    IvfcResult result = builder.finish(bytes, true);
    PROCESS_MEMORY_COUNTERS after = {};
    GetProcessMemoryInfo(GetCurrentProcess(), &after, sizeof(after));
    i64 levels = (i64)result.hashLevels.size();
    i64 peak = (i64)after.PeakPagefileUsage - (i64)before.PagefileUsage;
    printf("  %d GB of data: %lld MB of hash levels, %lld MB at the most\n", gigabytes, (long long)(levels >> 20), (long long)(peak >> 20));
    report("the size of the hash levels is what it should be", levels == IvfcBuilder::hashLevelsSize(bytes));
    report("and they only had to be in memory once", peak < levels + levels / 8, format("%lld MB", (long long)(peak >> 20)));
    return g_failures > 0 ? 1 : 0;
}

/// Everything from start to finish, with real games, on the pretend console.
int runReal(const std::string& keys, const std::string& folder, const std::string& shots, const std::vector<std::string>& nsps) {
    Keyset keyset;
    keyset.load(keys);

    std::string root = folder + "/real";
    sys::removeTree(root);
    sys::makeDirectories(root + "/sd/games");
    std::string consoleFolder = root + "/console";

    auto appearance = [&](SimConsole& console) {
        for (auto& path : nsps) {
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(path));
            set.group();
            for (auto& job : set.jobs) {
                u64 application = job.isBaseGame() ? job.main->meta.titleId : job.main->meta.linkedId();
                if (job.isBaseGame() && !job.titleName.empty())
                    console.setAppearance(application, job.titleName, readTitleIcon(job));
            }
        }
    };

    auto freshConsole = [&] {
        sys::removeTree(consoleFolder);
        wipeData(root);
        auto console = std::make_unique<SimConsole>(consoleFolder, keyset);
        console->install(nsps, Storage::Sd);
        appearance(*console);
        return console;
    };

    auto healthy = [&](SimConsole& console, const std::string& when) {
        std::string problem = console.checkConsistent(keyset);
        report(when + ": every game is whole", problem.empty(), problem);
        auto stray = console.strayFiles();
        report(when + ": nothing left lying around", stray.empty(), stray.empty() ? "" : stray[0]);
        report(when + ": no journal left", !sys::exists(root + "/data/journal/journal.txt"));
    };

    auto pcInstall = [&] {
        const char* share = getenv("HBP_TEST_PC_SHARE");
        if (getenv("HBP_TEST_PC") && *getenv("HBP_TEST_PC") && share && *share) {
            wipeData(root);
            const std::string& alone = nsps.size() > 2 ? nsps[2] : nsps[0];
            // The game, and its update and DLC when there are some, which the list groups under it.
            std::vector<std::pair<std::string, std::string>> shared{{alone, "from the pc.nsp"}};
            if (nsps.size() > 4) {
                shared.emplace_back(nsps[3], "from the pc update.nsp");
                shared.emplace_back(nsps[4], "from the pc dlc.nsp");
            }
            for (auto& file : shared) {
                Bytes bytes = readWhole(file.first);
                sys::File out(std::string(share) + "/" + file.second, sys::File::Mode::Create);
                out.write(0, bytes.data(), bytes.size());
            }
            // Nothing else on the list, and the settings as they are to begin with: PCs are looked in, and game files get installed.
            sys::removeTree(root + "/sd/games");
            sys::makeDirectories(root + "/sd/games");
            writeText(root + "/data/settings.txt", "welcomed=1\nwritelog=1\n");
            SimConsole console(root + "/console8", keyset);
            HostPlatform platform(root, keys, &console);
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToTab(person, app, 1);
            person.waitUntilIdle();
            person.settle();
            std::vector<int> fromPc;
            std::string listed;
            auto rows = app.snapshot().rows;
            for (int i = 0; i < (int)rows.size(); i++) {
                listed += rows[(size_t)i].name + " / " + rows[(size_t)i].detail + "; ";
                if (rows[(size_t)i].detail.find("On Test-PC") != std::string::npos)
                    fromPc.push_back(i);
            }
            report("what the PC shares is on the list of game files, as one game", fromPc.size() == 1, listed);
            person.shot("real_files_from_pc");
            std::string log = readText(root + "/data/log.txt");
            // An older PC can't say, and then the console reads the files itself, which the tests run both ways.
            printf("  The PC %s the game\n", log.find("Analyzed on the PC") != std::string::npos ? "analyzed" : "didn't analyze");
            report("and it's been analyzed", !fromPc.empty() && rows[(size_t)fromPc[0]].hasEstimate, listed);
            if (!fromPc.empty()) {
                for (int step = 0; step < fromPc[0]; step++)
                    person.press(ButtonDown);
            }
            chooseInMenu(person, app, "Install this game");
            bool asked = person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 120);
            report("installing from the PC asks first", asked, app.snapshot().dialog);
            double started = sys::now();
            person.press(ButtonA);
            person.waitUntilIdle();
            report("and it installs over the network", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
            printf("  Installing took %.1f s\n", sys::now() - started);
            std::string problem = console.checkConsistent(keyset);
            std::string what = problem;
            for (auto& line : app.snapshot().results)
                what += "; " + line;
            report("whole", problem.empty() && console.listApplications().size() == 1, what);
            person.press(ButtonA);
            person.waitUntilIdle();
            person.settle();
            bool stillThere = false;
            for (auto& row : app.snapshot().rows)
                stillThere = stillThere || row.detail.find("On Test-PC") != std::string::npos;
            report("once it's installed compressed, there's nothing left to do with it, so it's off the list", !stillThere, format("%d hidden", app.snapshot().compressedHidden));

            // The same PC on a cable as well, which the host build reaches over the network underneath.
            _putenv_s("HBP_TEST_PC_CABLE", "1");
            auto found = remote::peers();
            report("a PC on both the cable and the network is only looked in on the cable", found.size() == 1 && found[0].id == "USB",
                found.empty() ? "none" : found[0].id + format(" and %d more", (int)found.size() - 1));
            // What it shares is in a folder named after the one shared.
            std::string onCable = "pc:USB/";
            for (auto& entry : remote::list(onCable)) {
                if (entry.isDirectory) {
                    onCable += entry.name + "/" + shared[0].second;
                    break;
                }
            }
            remote::RemoteFile file(onCable, remote::fileSize(onCable));
            u8 first[4] = {};
            file.read(0, first, sizeof(first));
            report("and its games are read through the cable", memcmp(first, "PFS0", 4) == 0);
            auto withoutCable = remote::peers(false);
            report("with the cable left out of where to look, it's on the network again", withoutCable.size() == 1 && withoutCable[0].id == "Test-PC");
            _putenv_s("HBP_TEST_PC_CABLE", "");

            // The same game on the SD card as well, on a console without it: that's the copy used, and the PC's is noted on it.
            {
                Bytes bytes = readWhole(alone);
                sys::File out(root + "/sd/games/on the sd card.nsp", sys::File::Mode::Create);
                out.write(0, bytes.data(), bytes.size());
            }
            sys::removeTree(root + "/console9");
            SimConsole other(root + "/console9", keyset);
            HostPlatform otherPlatform(root, keys, &other);
            App both(otherPlatform);
            both.start();
            Person looking(both, shots);
            looking.waitUntilIdle();
            goToTab(looking, both, 1);
            looking.waitUntilIdle();
            looking.settle();
            std::string seen;
            int onSd = 0, onPc = 0, noted = 0;
            for (auto& row : both.snapshot().rows) {
                seen += row.name + " / " + row.detail + format(" (%d notes); ", row.notes);
                onSd += row.detail.find("On the SD card") != std::string::npos;
                onPc += row.detail.find("On Test-PC") != std::string::npos;
                noted += row.notes;
            }
            report("a game on the SD card and on a PC is listed once, from the SD card, with the other copy noted", onSd == 1 && onPc == 0 && noted > 0, seen);
            sys::removeFile(root + "/sd/games/on the sd card.nsp");
            for (auto& file : shared)
                sys::removeFile(std::string(share) + "/" + file.second);
        }
    };

    // Which way through to a PC wins, when there's more than one.
    {
        auto peer = [](const char* id, const char* machine) { return remote::Peer{id, id, nullptr, machine}; };
        std::vector<remote::Peer> both{peer("Desk", "Desk"), peer("Laptop", "Laptop"), peer("USB", "Desk")};
        remote::preferCable(both);
        report("a PC on the cable isn't listed over the network as well, while another PC is",
            both.size() == 2 && both[0].id == "Laptop" && both[1].id == "USB");
        std::vector<remote::Peer> unnamed{peer("Desk", "Desk"), peer("USB", "")};
        remote::preferCable(unnamed);
        report("a PC on the cable that hasn't said its name leaves the network ones alone", unnamed.size() == 2);
    }

    // While the list of game files waits on a PC that's stopped answering, what's asked for still goes first.
    auto waitingOnPc = [&] {
        auto console = freshConsole();
        sys::removeTree(root + "/sd/games");
        // Whatever an earlier test left the settings at, PCs on the network are looked in.
        for (const std::string& path : {root + "/data/settings.txt", root + "/config/settings.txt"}) {
            if (!sys::exists(path))
                continue;
            std::string text = readText(path);
            size_t at = text.find("frompcnetwork=0");
            if (at != std::string::npos)
                writeText(path, text.replace(at, 15, "frompcnetwork=1"));
        }
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            std::string before = getenv("HBP_TEST_PC") ? getenv("HBP_TEST_PC") : "";
            {
                SilentPc silent(25890);
                _putenv_s("HBP_TEST_PC", "127.0.0.1:25890");
                _putenv_s("HBP_TEST_PC_NAME", "Silent-PC");
                goToTab(person, app, 1);
                person.waitUntilIdle();
                chooseInMenu(person, app, "Refresh the list");
                person.waitFor([&] { return app.snapshot().scanning; }, 10);
                sys::sleepMs(1000);
                goToTab(person, app, 0);
                report("with games ticked and game files waiting on a PC", app.snapshot().scanning && app.snapshot().dialog.empty(),
                    format("%d connections to it, %s", silent.connections(), app.snapshot().dialog.c_str()));
                double started = sys::now();
                person.press(ButtonX);
                person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 60);
                double waited = sys::now() - started;
                report("compressing asks straight away all the same", app.snapshot().dialog.find("Compress") == 0 && waited < 5, format("%s after %.1f s", app.snapshot().dialog.c_str(), waited));
                person.press(ButtonB);
                _putenv_s("HBP_TEST_PC", before.c_str());
                _putenv_s("HBP_TEST_PC_NAME", "");
            }
            person.waitUntilIdle();
        }
    };

    // Only that, for when that's all that needs trying.
    if (getenv("HBP_TEST_ONLY_WAITING")) {
        waitingOnPc();
        printf("%d passed, %d failed\n", g_passes, g_failures);
        return g_failures > 0 ? 1 : 0;
    }

    // Only installing from a PC, for when that's all that needs trying.
    if (getenv("HBP_TEST_ONLY_PC")) {
        pcInstall();
        printf("%d passed, %d failed\n", g_passes, g_failures);
        return g_failures > 0 ? 1 : 0;
    }


    i64 totalBefore = 0;

    // The first run: the list, the analysis by itself, and compressing what's ticked.
    {
        auto console = freshConsole();
        for (u64 id : console->listApplications())
            totalBefore += console->describe(id).totalBytes();

        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitFor([&] { return app.snapshot().rows.size() > 0 || !app.busy(); });
        person.shot("real_first_look");
        bool sawAnalyzing = person.waitFor([&] { return app.snapshot().analyzing; }, 20);
        report("analysis starts by itself", sawAnalyzing);
        person.shot("real_analyzing");
        person.waitUntilIdle();
        person.shot("real_analyzed");

        App::Snapshot snapshot = app.snapshot();
        bool allKnown = !snapshot.rows.empty();
        for (auto& row : snapshot.rows)
            allKnown = allKnown && row.hasEstimate;
        report("every game has an answer", allKnown);
        report("the answers are remembered", sys::exists(root + "/data/analysis.txt"));
        report("the screen stayed awake while it worked", platform.workedAtAll);
        report("and may sleep again afterwards", !platform.working);

        person.press(ButtonX);
        report("compressing asks first", app.snapshot().dialog.find("Compress") == 0, app.snapshot().dialog);
        person.shot("real_confirm");
        person.press(ButtonA);
        report("the job screen shows", app.snapshot().screen == "Job");

        std::string seen;
        std::map<std::string, bool> shotOf;
        double start = sys::now();
        std::vector<std::pair<double, double>> bar;
        while (app.busy() && sys::now() - start < 3600) {
            person.frame();
            App::Snapshot during = app.snapshot();
            bar.emplace_back(sys::now() - start, during.overall);
            std::string label = during.label;
            if (!label.empty() && !shotOf[label]) {
                shotOf[label] = true;
                seen += label + "; ";
                std::string name = "real_job_" + std::to_string(shotOf.size());
                person.shot(name);
            }
            sys::sleepMs(5);
        }
        printf("  steps seen: %s\n", seen.c_str());
        {
            // However it's made up, the bar should be a fair idea of how much of the time has gone.
            double total = sys::now() - start;
            double worst = 0, longestStill = 0, stillSince = 0, last = 0;
            for (auto& point : bar) {
                if (point.second > last) {
                    last = point.second;
                    stillSince = point.first;
                }
                longestStill = std::max(longestStill, point.first - stillSince);
                worst = std::max(worst, fabs(point.second - point.first / total));
            }
            printf("  the bar was out by %.0f%% at the most, and stood still for %.1f s of %.1f s at the most\n", worst * 100, longestStill, total);
            report("the bar got to the end", !bar.empty() && bar.back().second > 0.9, format("%.2f", bar.empty() ? 0.0 : bar.back().second));
            report("and stayed a fair idea of how far along it was", worst < 0.45, format("out by %.0f%%", worst * 100));
        }
        person.settle();
        report("the results screen shows", app.snapshot().screen == "Results", app.snapshot().screen);
        person.shot("real_results");
        for (auto& line : app.snapshot().results)
            printf("  %s\n", line.c_str());
        person.press(ButtonA);
        person.settle();
        person.shot("real_after");
        healthy(*console, "after compressing");

        i64 totalAfter = 0;
        for (u64 id : console->listApplications())
            totalAfter += console->describe(id).totalBytes();
        printf("  %lld bytes before, %lld after\n", (long long)totalBefore, (long long)totalAfter);
        report("it took less room", totalAfter < totalBefore);
        std::string log = readText(root + "/data/log.txt");
        report("nothing unexpected in the log", log.find("Unexpected") == std::string::npos, log.substr(0, 400));
        size_t tests = 0;
        for (size_t at = log.find("files of the test copy back"); at != std::string::npos; at = log.find("files of the test copy back", at + 1))
            tests++;
        report("the console is only checked the once, however many games there are", tests == 1, format("%d times", (int)tests));
        report("nothing goes through a temporary folder", !sys::isDirectory(root + "/data/temp") || sys::list(root + "/data/temp").empty());
        report("the list is remembered for next time", sys::exists(root + "/data/list.cache"));
        size_t backed = 0;
        for (size_t at = log.find("files of "); at != std::string::npos; at = log.find("files of ", at + 1))
            backed++;
        printf("  %d lots of files read back\n", (int)backed);
        report("and the new copies were read back", backed >= 3, format("%d", (int)backed));
    }

    // Starting again: nothing to work out again, and nothing more to gain.
    {
        auto console = std::make_unique<SimConsole>(consoleFolder, keyset);
        appearance(*console);
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        report("the list is there from the first moment", !app.snapshot().rows.empty());
        Person person(app, shots);
        person.settle();
        person.waitUntilIdle();
        person.shot("real_second_start");
        report("and no names or icons had to be read again", console->controlReads == 0, format("%d", console->controlReads.load()));
        std::string log = readText(root + "/data/log.txt");
        report("the second start doesn't analyze again", log.find("Analyzing") == std::string::npos);
        App::Snapshot snapshot = app.snapshot();
        bool noneTicked = !snapshot.rows.empty();
        for (auto& row : snapshot.rows)
            noneTicked = noneTicked && !row.ticked;
        report("compressed games come up unticked", noneTicked);
        bool sayWhy = !snapshot.rows.empty(), anyCompressed = false;
        for (auto& row : snapshot.rows) {
            // A game with no game data to compress has nothing to gain, and isn't compressed either.
            sayWhy = sayWhy && (row.compressed ? row.note == "Already compressed" : row.note.find("Nothing to gain") != std::string::npos);
            anyCompressed = anyCompressed || row.compressed;
        }
        report("and say why", sayWhy && anyCompressed, snapshot.rows.empty() ? "" : snapshot.rows[0].note);

        // Doing it again by hand comes to the very same bytes, which mustn't cost the game anything.
        report("Y offers to tick everything", app.snapshot().tickHint == "Tick all", app.snapshot().tickHint);
        person.press(ButtonY);
        report("and does", !app.snapshot().rows.empty() && app.snapshot().rows[0].ticked && app.snapshot().toast == "Everything ticked", app.snapshot().toast);
        person.press(ButtonX);
        person.press(ButtonA);
        person.waitUntilIdle();
        person.shot("real_again");
        report("compressing it all again finishes", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        person.press(ButtonA);
        healthy(*console, "after compressing it all again");
    }

    // Opened for the first time on games that are already compressed, there's nothing to work out.
    {
        sys::removeFile(root + "/data/analysis.txt");
        sys::removeFile(root + "/config/analysis.txt");
        auto console = std::make_unique<SimConsole>(consoleFolder, keyset);
        appearance(*console);
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        std::string log = readText(root + "/data/log.txt");
        report("what's already compressed doesn't get sampled", log.find("Analyzing") != std::string::npos && log.find("samples of the game data") == std::string::npos, log.substr(log.size() > 400 ? log.size() - 400 : 0));
        App::Snapshot snapshot = app.snapshot();
        bool allSay = false;
        for (auto& row : snapshot.rows) {
            if (row.compressed)
                allSay = row.hasEstimate && !row.ticked && row.note == "Already compressed";
        }
        report("and comes up unticked, with the same check as what's just been compressed", allSay);
        person.shot("real_already_compressed");
    }

    // Left to itself, what's compressed stays off the list.
    {
        std::string kept = readText(root + "/data/settings.txt");
        std::string keptElsewhere = readText(root + "/config/settings.txt");
        writeText(root + "/data/settings.txt", "welcomed=1\ninstallgamefiles=0\nfrompcnetwork=0\nfrompccable=0\n");
        sys::removeFile(root + "/config/settings.txt");
        int games;
        {
            auto console = std::make_unique<SimConsole>(consoleFolder, keyset);
            appearance(*console);
            games = (int)console->listApplications().size();
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            App::Snapshot snapshot = app.snapshot();
            int hiddenNow = snapshot.compressedHidden;
            bool noneShown = hiddenNow > 0 && (int)snapshot.rows.size() + hiddenNow == games;
            for (auto& row : snapshot.rows)
                noneShown = noneShown && !row.compressed;
            report("games already compressed are hidden to begin with", noneShown, format("%d shown, %d hidden of %d", (int)snapshot.rows.size(), hiddenNow, games));
            person.shot("real_all_compressed_hidden");
            bool offered = goToMenuRow(person, app, format("Show the games that are done (%d)", hiddenNow));
            report("and the + menu offers to show them", offered);
            if (offered) {
                person.press(ButtonB);
                person.press(ButtonB);
            }
            goToOption(person, app, "Hide games that are done");
            person.press(ButtonRight);
            goToTab(person, app, 0);
            person.waitUntilIdle();
            report("the option brings them back", (int)app.snapshot().rows.size() == games && app.snapshot().compressedHidden == 0);
            person.shot("real_compressed_shown");
            report("and it's kept", readText(root + "/data/settings.txt").find("hidecompressed=0") != std::string::npos);
        }

        // What's just been compressed goes the same way.
        auto console = freshConsole();
        writeText(root + "/data/settings.txt", "welcomed=1\ninstallgamefiles=0\nminimumsaving=0\nfrompcnetwork=0\nfrompccable=0\n");
        sys::removeFile(root + "/config/settings.txt");
        games = (int)console->listApplications().size();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        int before = (int)app.snapshot().rows.size();
        person.press(ButtonX);
        person.press(ButtonA);
        person.waitUntilIdle();
        App::Snapshot snapshot = app.snapshot();
        report("what gets compressed goes off the list", before > 0 && snapshot.compressedHidden > 0 && (int)snapshot.rows.size() + snapshot.compressedHidden == before, format("%d before, %d shown, %d hidden, %s, %s", before, (int)snapshot.rows.size(), snapshot.compressedHidden, snapshot.results.empty() ? "" : snapshot.results[0].c_str(), readText(root + "/data/settings.txt").substr(0, 60).c_str()));
        report("and it's still in the results", !snapshot.results.empty());
        healthy(*console, "after hiding what was compressed");

        writeText(root + "/data/settings.txt", kept);
        sys::makeDirectories(root + "/config");
        writeText(root + "/config/settings.txt", keptElsewhere);
    }

    // A game too large for the memory there is gets said at the start, rather than hours later.
    {
        _putenv_s("HBP_TEST_WORK_MEMORY", "1000");
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        person.press(ButtonX);
        person.press(ButtonA);
        person.waitUntilIdle();
        std::string said;
        for (auto& line : app.snapshot().results)
            said += line + "\n";
        report("not enough memory for a game gets said", said.find("enough memory") != std::string::npos, said);
        healthy(*console, "after not having the memory");
        _putenv_s("HBP_TEST_WORK_MEMORY", "");
    }

    // Opened as an applet, it says so once, and what to do to have all the memory.
    {
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        platform.limited = true;
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 30);
        App::Snapshot said = app.snapshot();
        report("opened as an applet, it says so, and how to have all the memory", said.dialog == "Opened as an applet" && said.dialogText.find("hold R") != std::string::npos, said.dialog + ": " + said.dialogText);
        person.shot("real_applet");
        person.press(ButtonA);
        person.waitUntilIdle();
        person.settle();
        report("and only the once", app.snapshot().dialog.empty(), app.snapshot().dialog);
    }

    // A compressed game shouldn't have the console ask for it to be updated before it starts.
    {
        Bytes raw(0x20 + 0x10 + 0x20);
        raw[0x0C] = Cnmt::MetaApplication;
        wr16(&raw[0x0E], 0x10);
        wr32(&raw[0x2C], 0x50000);
        ContentMeta meta = ContentMeta::parse(raw, "a made up one");
        Bytes rebuilt = meta.rebuild(0, 0, {});
        report("a rebuilt game doesn't ask for a newer version of itself", rd32(&rebuilt[0x2C]) == 0, format("%u", rd32(&rebuilt[0x2C])));

        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        person.press(ButtonX);
        person.press(ButtonA);
        person.waitUntilIdle();
        report("and the console gets told what version counts now", !console->launchVersions.empty());
        person.press(ButtonA);
    }

    // Setting it up afresh by removing its folder brings up the welcome screen, whatever's kept in the other place.
    {
        auto console = freshConsole();
        sys::removeFile(root + "/data/settings.txt");
        sys::removeFile(root + "/data/settings.txt.new");
        sys::makeDirectories(root + "/config");
        writeText(root + "/config/settings.txt", "welcomed=1\ninstallgamefiles=0\nend\n");
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        report("a fresh folder brings up the welcome screen", app.showingWelcome());
        app.shutdown();
        startOver(root);
    }

    // Uninstalling and deleting from the + menu.
    {
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        size_t before = console->listApplications().size();
        chooseInMenu(person, app, "Uninstall this game");
        report("uninstalling asks first", app.snapshot().dialog.rfind("Uninstall ", 0) == 0, app.snapshot().dialog);
        person.shot("real_uninstall_confirm");
        person.press(ButtonA);
        report("and pressing A straight away keeps it", console->listApplications().size() == before);
        person.waitUntilIdle();
        chooseInMenu(person, app, "Uninstall this game");
        person.press(ButtonRight);
        person.press(ButtonA);
        person.waitUntilIdle();
        report("uninstalling takes it off the console", console->listApplications().size() + 1 == before, format("%d of %d", (int)console->listApplications().size(), (int)before));
        report("and says so", !app.snapshot().results.empty() && app.snapshot().results[0].find("Uninstalled") != std::string::npos, app.snapshot().results.empty() ? "" : app.snapshot().results[0]);
        person.press(ButtonA);
        report("and off the list", app.snapshot().rows.size() + 1 == before, format("%d", (int)app.snapshot().rows.size()));
        healthy(*console, "after uninstalling");

        auto nspsIn = [&](const std::string& folder) {
            int count = 0;
            for (auto& entry : sys::list(folder))
                count += endsWithNoCase(entry.name, ".nsp") ? 1 : 0;
            return count;
        };
        sys::makeDirectories(root + "/sd");
        writeText(root + "/sd/to delete.nsp", readText(nsps[0]));
        int files = nspsIn(root + "/sd");
        goToTab(person, app, 1);
        person.waitUntilIdle();
        chooseInMenu(person, app, "Delete this file");
        report("deleting a file asks first", app.snapshot().dialog.rfind("Delete ", 0) == 0, app.snapshot().dialog);
        person.press(ButtonRight);
        person.press(ButtonA);
        person.waitUntilIdle();
        report("and deletes it", nspsIn(root + "/sd") == files - 1, format("%d of %d", nspsIn(root + "/sd"), files));
        person.press(ButtonA);
        person.waitUntilIdle();
    }

    // Extracting what's already there is refused, rather than going over it.
    {
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        if (app.snapshot().tickHint == "Tick all")
            person.press(ButtonY);
        chooseInMenu(person, app, "Extract the ticked");
        person.waitUntilIdle();
        report("extracting works the first time", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle);
        person.press(ButtonA);
        person.waitUntilIdle();
        chooseInMenu(person, app, "Extract the ticked");
        person.waitUntilIdle();
        std::string said;
        for (auto& line : app.snapshot().results)
            said += line + "; ";
        report("and the second time says it's been done", said.find("It's already in") != std::string::npos, said);
        person.press(ButtonA);
        person.waitUntilIdle();
        sys::removeTree(root + "/output/extracted");
    }

    // Refreshing one list while another comes up leaves that one to finish afterwards.
    {
        sys::makeDirectories(root + "/sd/many");
        for (int i = 0; i < 1500; i++)
            writeText(root + "/sd/many/" + format("nothing %d.nsp", i), "not a game");
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        person.press(ButtonR);
        person.waitFor([&] { return app.snapshot().scanning; }, 5);
        person.press(ButtonL);
        chooseInMenu(person, app, "Refresh the list");
        person.waitUntilIdle();
        std::string log = readText(root + "/data/log.txt");
        report("the list that was left comes up by itself", log.find("of them opened") != std::string::npos && log.find("1500 game files") != std::string::npos, log.substr(log.size() > 400 ? log.size() - 400 : 0));
        sys::removeTree(root + "/sd/many");
    }

    // Everything ticked in one go, which it spells out first, as much of a list comes up ticked by itself.
    {
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        size_t before = console->listApplications().size();
        if (app.snapshot().tickHint == "Tick all")
            person.press(ButtonY);
        chooseInMenu(person, app, "Uninstall the ticked games");
        report("uninstalling several says how many", app.snapshot().dialog == format("Uninstall %d games?", (int)before), app.snapshot().dialog);
        person.shot("real_uninstall_many");
        person.press(ButtonA);
        report("and pressing A straight away keeps them", console->listApplications().size() == before);
        person.waitUntilIdle();
        chooseInMenu(person, app, "Uninstall the ticked games");
        person.press(ButtonRight);
        person.press(ButtonA);
        person.waitUntilIdle();
        report("and it takes them all", console->listApplications().empty(), format("%d left", (int)console->listApplications().size()));
        report("with a line for each", app.snapshot().results.size() == before, format("%d", (int)app.snapshot().results.size()));
        person.press(ButtonA);
        report("and they're off the list", app.snapshot().rows.empty());
        healthy(*console, "after uninstalling them all");
    }

    // Settings and the menu stay short to begin with.
    {
        auto console = freshConsole();
        writeText(root + "/data/settings.txt", "welcomed=1\ninstallgamefiles=0\nfrompcnetwork=0\nfrompccable=0\n");
        sys::removeFile(root + "/config/settings.txt");
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        goToTab(person, app, SettingsTab);
        person.shot("real_options_short");
        size_t few = app.snapshot().options.size();
        report("there's no log to begin with", !sys::exists(root + "/data/log.txt"));
        report("the settings are short to begin with", few <= 8 && optionValue(app, "Compression level") == "(no such option)", format("%d", (int)few));
        goToOption(person, app, "More settings");
        person.press(ButtonA);
        person.settle();
        report("and the rest are there when asked for", app.snapshot().options.size() > few && optionValue(app, "Compression level") != "(no such option)", format("%d", (int)app.snapshot().options.size()));
        report("which is kept", readText(root + "/data/settings.txt").find("moresettings=1") != std::string::npos);
        goToTab(person, app, 0);
        person.press(ButtonPlus);
        report("the + menu is short too", app.snapshot().menu.size() <= 7, format("%d", (int)app.snapshot().menu.size()));
        person.shot("real_menu_short");
        person.press(ButtonB);
        startOver(root);
    }

    // A folder only goes if it's a game, and then all of it does.
    {
        auto console = freshConsole();
        std::string extractedTo = root + "/output/extracted";
        std::string folder;
        {
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(nsps[0]));
            set.group();
            folder = extractTitle(set.jobs.at(0), set, extractedTo).path;
        }
        writeText(root + "/output/made from it [LZ4].nsp", readText(nsps[0]));
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        goToTab(person, app, 1);
        person.waitUntilIdle();
        chooseInMenu(person, app, "Delete this folder");
        person.press(ButtonRight);
        person.press(ButtonA);
        person.waitUntilIdle();
        report("deleting a folder takes all of it", !sys::exists(folder), folder);
        person.press(ButtonA);
        person.waitUntilIdle();
        report("and what was made from it doesn't take its place on the list", app.snapshot().rows.empty() && sys::exists(root + "/output/made from it [LZ4].nsp"), format("%d", (int)app.snapshot().rows.size()));
    }

    // Going to another list doesn't hold up what's being worked out for this one, even while that list comes up.
    {
        sys::removeFile(root + "/data/analysis.txt");
        sys::removeFile(root + "/config/analysis.txt");
        forgetIndex(root);
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        report("analyzing starts", person.waitFor([&] { return app.snapshot().analyzing; }, 60));
        goToTab(person, app, 1);
        person.waitUntilIdle();
        std::string log = readText(root + "/data/log.txt");
        report("and isn't stopped by the next list coming up", log.find("Stopped analyzing") == std::string::npos, log.substr(log.size() > 500 ? log.size() - 500 : 0));
        goToTab(person, app, 0);
        App::Snapshot back = app.snapshot();
        bool allKnown = !back.rows.empty();
        for (auto& row : back.rows)
            allKnown = allKnown && row.hasEstimate;
        report("so it's all done when coming back", allKnown && !back.analyzing);
        healthy(*console, "after analyzing beside another list");
    }

    // Pressing X while it's still analyzing means getting on with it.
    {
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitFor([&] { return app.snapshot().analyzing || !app.busy(); }, 60);
        InputState down;
        down.held = ButtonX;
        person.frame(down);
        person.frame();
        bool asked = person.waitFor([&] { return app.snapshot().dialog.find("Compress") == 0; }, 60);
        report("compressing during the analysis stops it, and asks", asked, app.snapshot().dialog);
        person.shot("real_x_while_analyzing");
        person.press(ButtonB);
        person.settle();
        report("saying no leaves the analysis stopped", !app.busy());
    }

    // The firmware refuses: everything stays.
    {
        auto console = freshConsole();
        console->setFirmwareRefuses(true);
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        person.press(ButtonX);
        person.press(ButtonA);
        person.waitUntilIdle();
        report("a firmware that refuses stops everything at once", app.snapshot().resultsTitle == "Nothing was changed" && app.snapshot().results.empty(), app.snapshot().resultsTitle);
        report("and says what it takes", app.snapshot().resultsDetail.find("sigpatches") != std::string::npos, app.snapshot().resultsDetail);
        person.shot("real_refused");
        healthy(*console, "after the firmware refused");
        i64 total = 0;
        for (u64 id : console->listApplications())
            total += console->describe(id).totalBytes();
        report("and the games are the size they were", total == totalBefore);
    }

    // No room.
    {
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        console->setFree(Storage::Sd, 1 << 20);
        person.press(ButtonX);
        report("no room gets said before anything starts", app.snapshot().dialog == "There isn't enough room", app.snapshot().dialog);
        report("and nothing has started", app.snapshot().screen == "Main");
        person.shot("real_noroom");
        person.press(ButtonA);
        // What one game frees makes room for the next.
        console->setFree(Storage::Sd, 120LL << 20);
        person.press(ButtonX);
        App::Snapshot asked = app.snapshot();
        report("what one frees counts towards the next", asked.dialog.find("Compress these") == 0 && asked.dialogText.find("left out") == std::string::npos, asked.dialog + " " + asked.dialogText);
        person.press(ButtonB);

        // Saving NSPs frees nothing, so there's only room for some.
        goToOption(person, app, "Installed games");
        person.press(ButtonRight);
        goToTab(person, app, 0);
        person.settle();
        console->setFree(Storage::Sd, 100LL << 20);
        person.press(ButtonX);
        asked = app.snapshot();
        report("what doesn't fit gets said", asked.dialog.find("Compress this") == 0 && asked.dialogText.find("left out") != std::string::npos, asked.dialog + " " + asked.dialogText);
        printf("  with little room: %s\n", asked.dialogText.c_str());
        person.shot("real_room_for_some");
        person.press(ButtonB);
        console->setFree(Storage::Sd, -1);
        healthy(*console, "after there was no room");
        startOver(root);
    }

    // Stopping part way through.
    {
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        person.press(ButtonX);
        person.press(ButtonA);
        person.waitFor([&] { return app.snapshot().label.find("Compressing") == 0 || !app.busy(); }, 120);
        InputState down;
        down.held = ButtonB;
        person.frame(down);
        person.frame();
        person.shot("real_stop_dialog");
        report("stopping asks first", app.snapshot().dialog == "Stop?", app.snapshot().dialog);
        // The choices run left to right, and the last is to stop now.
        person.press(ButtonRight);
        person.press(ButtonRight);
        person.press(ButtonA);
        person.waitUntilIdle();
        report("it stops", app.snapshot().screen == "Results", app.snapshot().screen);
        person.shot("real_stopped");
        healthy(*console, "after stopping");
    }

    // The power goes while it's working, and the next start puts it right.
    {
        auto console = freshConsole();
        i64 before = 0;
        for (u64 id : console->listApplications())
            before += console->describe(id).totalBytes();
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            person.press(ButtonX);
            person.press(ButtonA);
            person.waitFor([&] { return app.snapshot().label.find("Checking every byte") == 0 || !app.busy(); }, 600);
            report("a game gets to the point of being read back", app.busy());
            console->losePowerAt(1);
            person.waitUntilIdle();
        }
        console->losePowerAt(0);
        auto restarted = std::make_unique<SimConsole>(consoleFolder, keyset);
        appearance(*restarted);
        HostPlatform platform(root, keys, restarted.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.settle();
        person.waitUntilIdle();
        report("the next start says what it did", app.snapshot().dialog == "Last time", app.snapshot().dialog);
        person.shot("real_after_power_cut");
        person.press(ButtonA);
        healthy(*restarted, "after a power cut");
    }

    // The rule, and ticking by hand.
    {
        auto console = freshConsole();
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
        }

        // These games save too much for the rule to apply, so the answers are made smaller than they were.
        std::string answers = readText(root + "/data/analysis.txt"), changed;
        size_t position = 0;
        while (position < answers.size()) {
            size_t end = answers.find('\n', position);
            std::string line = answers.substr(position, end - position);
            position = end + 1;
            long long input, output, margin, romfs, stored;
            size_t tab = line.find('\t');
            if (tab != std::string::npos && sscanf(line.c_str() + tab + 1, "%lld %lld %lld %lld %lld", &input, &output, &margin, &romfs, &stored) == 5)
                line = line.substr(0, tab + 1) + format("%lld %lld %lld %lld %lld 0", input, input * 8 / 10, margin, romfs, stored);
            changed += line + "\n";
        }
        writeText(root + "/data/analysis.txt", changed);

        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        goToOption(person, app, "Minimum saving");
        for (int i = 0; i < 8; i++)
            person.press(ButtonRight);
        person.shot("real_options_minimum");
        goToTab(person, app, 0);
        person.settle();
        App::Snapshot snapshot = app.snapshot();
        bool noneTicked = !snapshot.rows.empty();
        for (auto& row : snapshot.rows)
            noneTicked = noneTicked && !row.ticked;
        report("a minimum of 50% leaves out games that save less", noneTicked);
        report("the setting is kept", readText(root + "/data/settings.txt").find("minimumsaving=50") != std::string::npos);
        person.shot("real_minimum");
        person.press(ButtonA);
        snapshot = app.snapshot();
        report("a game can still be ticked by hand", !snapshot.rows.empty() && snapshot.rows[0].ticked);

        // Hiding one, and bringing it back.
        size_t count = snapshot.rows.size();
        chooseInMenu(person, app, "Hide this game");
        person.settle();
        report("a game can be hidden", app.snapshot().rows.size() + 1 == count);
    }
    {
        auto console = std::make_unique<SimConsole>(consoleFolder, keyset);
        appearance(*console);
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.settle();
        person.waitUntilIdle();
        size_t hiddenCount = app.snapshot().rows.size();
        report("it stays hidden the next time", hiddenCount + 1 == console->listApplications().size());
        report("the minimum is still set", readText(root + "/data/settings.txt").find("minimumsaving=50") != std::string::npos);
        person.press(ButtonPlus);
        person.shot("real_menu_hidden");
        person.press(ButtonB);
        chooseInMenu(person, app, "Show the hidden");
        person.waitUntilIdle();
        report("hidden games can be brought back", app.snapshot().rows.size() == console->listApplications().size());
    }

    // Game files on the card: saved in the NSP folder, then installed.
    {
        auto console = freshConsole();
        const std::string& fileToKeep = nsps.size() > 2 ? nsps[2] : nsps[0];
        std::string kept = root + "/sd/games/" + fileToKeep.substr(fileToKeep.find_last_of("/\\") + 1);
        {
            sys::File in(fileToKeep, sys::File::Mode::Read);
            sys::File out(kept, sys::File::Mode::Create);
            Bytes buffer(1 << 20);
            i64 size = in.size();
            for (i64 done = 0; done < size;) {
                size_t n = (size_t)std::min<i64>((i64)buffer.size(), size - done);
                in.read(done, buffer.data(), n);
                out.write(done, buffer.data(), n);
                done += (i64)n;
            }
        }
        u8 before[32];
        Bytes sourceBytes = readWhole(kept);
        sha256(sourceBytes.data(), sourceBytes.size(), before);

        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        person.press(ButtonR);
        person.shot("real_files_scanning");
        person.waitUntilIdle();
        person.settle();
        person.shot("real_files");
        App::Snapshot snapshot = app.snapshot();
        report("the file is listed", snapshot.rows.size() == 1, format("%d", (int)snapshot.rows.size()));

        person.press(ButtonX);
        person.shot("real_files_confirm");
        person.press(ButtonA);
        person.waitUntilIdle();
        person.shot("real_files_results");
        report("saving an NSP finishes", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle);
        person.press(ButtonA);

        bool found = false;
        for (auto& entry : sys::list(root + "/output"))
            found = found || entry.name.find("[LZ4]") != std::string::npos;
        report("the NSP is in the NSP folder", found);
        Bytes after = readWhole(kept);
        u8 afterHash[32];
        sha256(after.data(), after.size(), afterHash);
        report("the file itself isn't changed", memcmp(before, afterHash, 32) == 0);

        // A look at it again only takes going through the folders.
        {
            std::string first = readText(root + "/data/log.txt");
            report("what's new got opened", first.find(", 1 of them opened") != std::string::npos, first.substr(first.size() > 400 ? first.size() - 400 : 0));
            report("what's known about it is kept", sys::exists(root + "/data/files.cache"));
        }

        // It's on the list of what's in the folders, as that's where it went.
        goToTab(person, app, 1);
        person.waitUntilIdle();
        person.settle();
        report("what's been saved doesn't make another entry for the same game", app.snapshot().rows.size() == 1, format("%d", (int)app.snapshot().rows.size()));
        person.shot("real_folders_with_nsp");

        // Installing it now, over the game that's installed with its update, which is newer than the file.
        goToOption(person, app, "Game files and folders");
        person.press(ButtonRight);
        goToTab(person, app, 1);
        person.waitUntilIdle();
        person.settle();
        report("the choice to install is kept", readText(root + "/data/settings.txt").find("installgamefiles=1") != std::string::npos);
        person.press(ButtonY);  // Tick everything: the file is done, but it hasn't been installed.
        person.press(ButtonX);
        person.shot("real_install_confirm");
        person.press(ButtonA);
        person.waitUntilIdle();
        person.shot("real_install_refused");
        std::string joined;
        for (auto& line : app.snapshot().results)
            joined += line + "\n";
        report("a file older than what's installed is refused", joined.find("newer version") != std::string::npos, joined);
        person.press(ButtonA);
        healthy(*console, "after refusing to install");
    }

    // Installing on a console that hasn't got the game.
    {
        sys::removeTree(root + "/console2");
        sys::removeTree(root + "/output");
        sys::removeFile(root + "/data/list.cache");
        SimConsole console(root + "/console2", keyset);
        HostPlatform platform(root, keys, &console);
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        person.shot("real_nothing_installed");
        person.press(ButtonR);
        person.waitUntilIdle();
        person.settle();
        person.press(ButtonX);
        person.press(ButtonA);
        person.waitUntilIdle();
        person.shot("real_install_results");
        report("installing finishes", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        person.press(ButtonA);
        person.press(ButtonL);
        person.settle();
        person.shot("real_installed_now");
        report("and the game is listed as installed", app.snapshot().rows.size() + app.snapshot().compressedHidden == 1, format("%d", (int)app.snapshot().rows.size()));
        std::string problem = console.checkConsistent(keyset);
        report("after installing: it's whole", problem.empty(), problem);
        auto stray = console.strayFiles();
        report("after installing: nothing left lying around", stray.empty(), stray.empty() ? "" : stray[0]);
        startOver(root);
    }

    // Deleting a file once it's installed, as long as nothing else in it still needs it.
    {
        sys::removeTree(root + "/console3");
        sys::removeFile(root + "/data/list.cache");
        std::string file;
        for (auto& entry : sys::list(root + "/sd/games"))
            file = root + "/sd/games/" + entry.name;
        SimConsole console(root + "/console3", keyset);
        HostPlatform platform(root, keys, &console);
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        goToOption(person, app, "Game files and folders");
        person.press(ButtonRight);
        goToOption(person, app, "Once a file is installed");
        person.press(ButtonRight);
        person.shot("real_options_delete");
        goToTab(person, app, 0);
        report("the choice to delete is kept", readText(root + "/data/settings.txt").find("deleteinstalled=1") != std::string::npos);
        person.press(ButtonR);
        person.waitUntilIdle();
        person.settle();
        person.press(ButtonX);
        report("it says the file will go", app.snapshot().dialogText.find("deleted") != std::string::npos, app.snapshot().dialogText);
        person.shot("real_delete_confirm");
        Bytes source = readWhole(file);
        person.press(ButtonA);
        person.waitUntilIdle();
        person.shot("real_delete_results");
        report("installing and deleting finishes", app.snapshot().resultsTitle == "Finished" && app.snapshot().resultsDetail.find("deleted") != std::string::npos, app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        report("the file is gone", !sys::exists(file));
        person.press(ButtonA);
        person.waitUntilIdle();
        report("and off the list", app.snapshot().rows.empty(), format("%d", (int)app.snapshot().rows.size()));
        person.press(ButtonL);
        person.waitUntilIdle();
        report("while the game is installed", app.snapshot().rows.size() == 1, format("%d", (int)app.snapshot().rows.size()));
        std::string problem = console.checkConsistent(keyset);
        report("and whole", problem.empty(), problem);

        // Back as it was for what comes next.
        sys::File out(file, sys::File::Mode::Create);
        out.write(0, source.data(), source.size());
        startOver(root);
    }

    // Extracting.
    {
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        chooseInMenu(person, app, "Extract the ticked");
        person.shot("real_extracting");
        person.waitUntilIdle();
        person.shot("real_extracted");
        report("extracting finishes", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle);
        report("the files are there", sys::isDirectory(root + "/output/extracted"));
        healthy(*console, "after extracting");
    }

    // Checking for damage.
    {
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        auto checkAll = [&] {
            chooseInMenu(person, app, "Check the ticked");
            person.waitUntilIdle();
        };
        checkAll();
        person.shot("real_checked");
        report("whole games get reported as whole", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        person.press(ButtonA);

        console->setCardCorrupts(true);
        checkAll();
        console->setCardCorrupts(false);
        person.shot("real_checked_damaged");
        report("damage gets found", app.snapshot().resultsTitle.find("problems") != std::string::npos, app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        person.press(ButtonA);
    }

    // An extracted game gets built into an NSP again.
    {
        auto console = freshConsole();
        sys::removeTree(root + "/sd/games");
        sys::removeTree(root + "/output");
        sys::removeFile(root + "/data/list.cache");
        std::string extractedInto = root + "/output/extracted";
        {
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(nsps.size() > 2 ? nsps[2] : nsps[0]));
            set.group();
            extractTitle(set.jobs.at(0), set, extractedInto);
        }
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        person.press(ButtonR);
        person.waitUntilIdle();
        report("an extracted game is one game on the list of game files, not the files inside it", app.snapshot().rows.size() == 1, format("%d", (int)app.snapshot().rows.size()));
        person.waitUntilIdle();
        person.settle();
        person.shot("real_extracted_listed");
        App::Snapshot snapshot = app.snapshot();
        report("the extracted game is listed", snapshot.rows.size() == 1 && snapshot.rows[0].hasEstimate, format("%d", (int)snapshot.rows.size()));
        person.press(ButtonX);
        person.shot("real_extracted_confirm");
        person.press(ButtonA);
        person.waitUntilIdle();
        person.shot("real_extracted_built");
        report("building it finishes", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        person.press(ButtonA);

        std::string built;
        for (auto& entry : sys::list(root + "/output")) {
            if (endsWithNoCase(entry.name, ".nsp"))
                built = root + "/output/" + entry.name;
        }
        report("and there's an NSP of it", !built.empty());
        if (!built.empty()) {
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(built));
            set.group();
            report("that opens as a game", set.jobs.size() == 1 && set.problems.empty());
        }
        person.waitUntilIdle();
        person.settle();
        report("which doesn't go on the list beside it", app.snapshot().rows.size() == 1, format("%d", (int)app.snapshot().rows.size()));

        // A folder chosen with the file explorer, on the USB drive that's just been plugged in.
        std::string onUsb = root + "/usb/my games";
        extractedInto = onUsb;
        {
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(nsps.size() > 2 ? nsps[2] : nsps[0]));
            set.group();
            extractTitle(set.jobs.at(0), set, extractedInto);
        }
        platform.usb = true;
        platform.usbChanged = true;
        person.settle();
        report("a USB drive being plugged in gets noticed", app.snapshot().toast == "USB drive found", app.snapshot().toast);
        person.press(ButtonStickL);
        report("the left stick does nothing now", app.snapshot().screen == "Main", app.snapshot().screen);
        person.press(ButtonMinus);
        report("- does nothing on the lists any more", app.snapshot().screen == "Main" && app.snapshot().menu.empty(), app.snapshot().screen);
        goToMenuRow(person, app, "Where to look");
        person.press(ButtonA);
        // On the list of game files, it says where to look, which can be one folder chosen with the file explorer.
        auto where = app.snapshot().menu;
        std::string listedWhere;
        for (auto& item : where)
            listedWhere += item + "; ";
        report("the + menu says where to look",listedWhere.find("The SD card: Shown") != std::string::npos && listedWhere.find("A PC on the USB cable") != std::string::npos, listedWhere);
        person.shot("real_where_to_look");
        for (int step = 0; step < 4; step++)
            person.press(ButtonDown);
        person.press(ButtonA);
        report("and one folder can be chosen with the file explorer", app.snapshot().screen == "Browse", app.snapshot().screen);
        person.shot("real_browse_folder");
        while (!app.snapshot().browsing.empty())
            person.press(ButtonB);
        int drivesShown = 0;
        for (auto& entry : app.snapshot().browseEntries)
            drivesShown += entry.rfind("PC", 0) == 0 ? 0 : 1;
        report("the drives are all there to start from", drivesShown == 2, format("%d", (int)app.snapshot().browseEntries.size()));
        person.shot("real_browse_drives");
        person.press(ButtonDown);
        person.press(ButtonA);
        report("going into the USB drive", app.snapshot().browseEntries.size() == 1 && app.snapshot().browseEntries[0] == "my games", app.snapshot().browseEntries.empty() ? "" : app.snapshot().browseEntries[0]);
        person.press(ButtonA);
        App::Snapshot inside = app.snapshot();
        report("and folders holding a game are marked", inside.browseEntries.size() == 1 && inside.browseEntries[0].find("Extracted game") != std::string::npos, inside.browseEntries.empty() ? "" : inside.browseEntries[0]);
        person.shot("real_browse_game");
        person.press(ButtonX);
        report("choosing it goes back", app.snapshot().screen == "Main", app.snapshot().screen);
        person.waitUntilIdle();
        person.settle();
        snapshot = app.snapshot();
        report("and lists what's in it", snapshot.rows.size() == 1 && !snapshot.rows.empty(), format("%d", (int)snapshot.rows.size()));
        person.shot("real_folder_on_usb");
        report("the choice is kept", readText(root + "/data/settings.txt").find("filesfolder=" + onUsb) != std::string::npos);

        // NSPs going onto the USB drive too.
        goToOption(person, app, "NSP files go in");
        person.press(ButtonA);
        report("the NSP folder gets chosen in the file explorer", app.snapshot().screen == "Browse", app.snapshot().screen);
        while (!app.snapshot().browsing.empty())
            person.press(ButtonB);
        person.press(ButtonDown);
        person.press(ButtonA);
        person.press(ButtonX);
        report("and it goes back to the options", app.snapshot().screen == "Options", app.snapshot().screen);
        person.shot("real_options_folders");
        goToTab(person, app, 1);
        person.waitUntilIdle();
        person.press(ButtonX);
        report("the room gets looked at on that drive", app.snapshot().dialogText.find(root + "/usb") != std::string::npos, app.snapshot().dialogText);
        person.press(ButtonA);
        person.waitUntilIdle();
        report("building it there finishes", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        person.press(ButtonA);
        bool onDrive = false;
        for (auto& entry : sys::list(root + "/usb"))
            onDrive = onDrive || endsWithNoCase(entry.name, ".nsp");
        report("and the NSP is on the drive", onDrive);

        platform.usb = false;
        platform.usbChanged = true;
        person.settle();
        report("a drive being taken out gets noticed", app.snapshot().toast == "USB drive taken out", app.snapshot().toast);
        person.waitUntilIdle();
        sys::removeTree(root + "/usb");
        startOver(root);
        sys::removeFile(root + "/data/list.cache");
    }

    // With compression off it's a repack, called what it is, which can go in place of what's installed.
    {
        auto console = freshConsole();
        i64 before = 0;
        for (u64 id : console->listApplications())
            before += console->describe(id).totalBytes();
        sys::removeTree(root + "/output");
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        goToOption(person, app, "Compression");
        auto shown = [&](const char* label) {
            for (auto& option : app.snapshot().options) {
                if (option.rfind(std::string(label) + ":", 0) == 0)
                    return true;
            }
            return false;
        };
        report("with compression on, its own settings are there", shown("Minimum saving") && shown("Compression level") && shown("Block size"));
        person.press(ButtonRight);
        person.shot("real_options_no_compression");
        App::Snapshot off = app.snapshot();
        report("with it off, they're gone, and the focus stays on it", !shown("Minimum saving") && !shown("Compression level") && !shown("Block size") &&
            off.optionFocus >= 0 && off.optionFocus < (int)off.options.size() && off.options[(size_t)off.optionFocus].rfind("Compression:", 0) == 0,
            off.optionFocus >= 0 && off.optionFocus < (int)off.options.size() ? off.options[(size_t)off.optionFocus] : "nothing");
        goToTab(person, app, 0);
        person.waitUntilIdle();
        person.settle();
        person.shot("real_repack_analyzed");
        person.press(ButtonX);
        report("repacking asks first", app.snapshot().dialog.find("Repack") == 0, app.snapshot().dialog);
        report("and says what happens to the game", app.snapshot().dialogText.find("replaced by a repacked copy") != std::string::npos, app.snapshot().dialogText);
        person.shot("real_repack_replace_confirm");
        person.press(ButtonA);
        person.waitUntilIdle();
        report("repacking what's installed finishes", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        for (auto& line : app.snapshot().results)
            printf("  %s\n", line.c_str());
        person.shot("real_repack_replaced");
        person.press(ButtonA);
        healthy(*console, "after repacking what's installed");
        i64 after = 0;
        for (u64 id : console->listApplications())
            after += console->describe(id).totalBytes();
        printf("  %lld bytes before, %lld after\n", (long long)before, (long long)after);
        report("a game with an update comes out smaller", after < before);
        std::string log = readText(root + "/data/log.txt");
        report("the console's check is what says it takes sigpatches", log.find("files of the test copy back") != std::string::npos);
    }

    // Only saving an NSP leaves what's installed as it is.
    {
        auto console = freshConsole();
        i64 before = 0;
        for (u64 id : console->listApplications())
            before += console->describe(id).totalBytes();
        sys::removeTree(root + "/output");
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        goToOption(person, app, "Compression");
        person.press(ButtonRight);
        goToOption(person, app, "Installed games");
        person.press(ButtonRight);
        goToTab(person, app, 0);
        person.waitUntilIdle();
        person.settle();
        report("the button is called Repack", app.snapshot().actionWord == "Repack", app.snapshot().actionWord);
        person.shot("real_repack_list");
        person.press(ButtonX);
        report("and so is what it asks", app.snapshot().dialog.find("Repack") == 0, app.snapshot().dialog);
        person.shot("real_repack_confirm");
        person.press(ButtonA);
        std::string title;
        person.waitFor([&] { return app.snapshot().screen == "Job"; }, 30);
        person.shot("real_repacking");
        person.waitUntilIdle();
        report("repacking finishes", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        report("and calls it that", app.snapshot().resultsDetail.find("repacked") != std::string::npos, app.snapshot().resultsDetail);
        person.shot("real_repacked");
        person.press(ButtonA);
        i64 after = 0;
        for (u64 id : console->listApplications())
            after += console->describe(id).totalBytes();
        report("what's installed isn't touched", before == after);
        std::string repacked;
        for (auto& entry : sys::list(root + "/output")) {
            if (endsWithNoCase(entry.name, ".nsp"))
                repacked = root + "/output/" + entry.name;
        }
        report("and there's an NSP", !repacked.empty() && repacked.find("[LZ4]") == std::string::npos, repacked);
        if (!repacked.empty()) {
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(repacked));
            set.group();
            report("that opens as a game", set.jobs.size() == 1 && set.problems.empty());
        }
        std::string log = readText(root + "/data/log.txt");
        report("with no need for the console's check", log.find("files of the test copy back") == std::string::npos);
        startOver(root);
        sys::removeFile(root + "/data/list.cache");
    }

    // With compression off, a game with no update has nothing to gain, but it may be ticked for something else.
    {
        const std::string& alone = nsps.size() > 2 ? nsps[2] : nsps[0];
        sys::removeTree(root + "/sd/games");
        sys::makeDirectories(root + "/sd/games");
        std::string copied = root + "/sd/games/alone.nsp";
        {
            Bytes bytes = readWhole(alone);
            sys::File out(copied, sys::File::Mode::Create);
            out.write(0, bytes.data(), bytes.size());
        }
        sys::removeTree(root + "/output");
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        goToOption(person, app, "Compression");
        person.press(ButtonRight);
        goToTab(person, app, 1);
        person.waitUntilIdle();
        person.settle();
        // With nothing saved to go by, Y only goes between all and none, and four times round leaves it as it was.
        std::string offered;
        for (int i = 0; i < 4; i++) {
            offered += app.snapshot().tickHint + "; ";
            person.press(ButtonY);
        }
        report("with compression off, Y doesn't offer what's worth it", offered.find("Worth it") == std::string::npos && !app.snapshot().rows.empty() && app.snapshot().rows[0].ticked, offered);
        person.press(ButtonX);
        App::Snapshot asked = app.snapshot();
        report("a game with no update gets asked about", asked.dialog.find("no update") != std::string::npos, asked.dialog);
        person.shot("real_no_update_question");
        person.press(ButtonB);
        report("cancelling leaves the ticks as they were", !app.snapshot().rows.empty() && app.snapshot().rows[0].ticked && app.snapshot().dialog.empty(), app.snapshot().dialog);
        person.press(ButtonX);
        person.press(ButtonA);
        report("unticking it leaves nothing to repack", app.snapshot().dialog.find("Nothing left") != std::string::npos && !app.snapshot().rows[0].ticked, app.snapshot().dialog);
        person.press(ButtonA);
        person.press(ButtonY);
        person.press(ButtonX);
        person.press(ButtonRight);
        person.press(ButtonA);
        report("it can be repacked anyway", app.snapshot().dialog.find("Repack this") == 0, app.snapshot().dialog);
        person.press(ButtonB);
        report("and installing it doesn't get asked about at all", chooseInMenu(person, app, "Install the ticked") && app.snapshot().dialog.find("Install this") == 0, app.snapshot().dialog);
        person.press(ButtonB);
        startOver(root);
        sys::removeFile(root + "/data/list.cache");
    }

    // A game, its update and its DLC are one row, and so are copies of them. A compressed copy as new as what it was made from takes their place.
    if (nsps.size() > 4) {
        std::string games = root + "/sd/games";
        auto copyTo = [&](const std::string& from, const std::string& to) {
            Bytes bytes = readWhole(from);
            sys::File out(to, sys::File::Mode::Create);
            out.write(0, bytes.data(), bytes.size());
        };
        sys::removeTree(games);
        sys::makeDirectories(games + "/copy");
        copyTo(nsps[2], games + "/base.nsp");
        copyTo(nsps[3], games + "/update.nsp");
        copyTo(nsps[4], games + "/dlc.nsp");
        copyTo(nsps[2], games + "/copy/base.nsp");
        {
            wipeData(root);
            auto console = freshConsole();
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToTab(person, app, 1);
            person.waitUntilIdle();
            person.settle();
            auto rows = app.snapshot().rows;
            report("a game, its update and its DLC are one row", rows.size() == 1 && rows[0].detail.find("1 DLC") != std::string::npos && rows[0].detail.find("update") != std::string::npos,
                   rows.empty() ? format("%d rows", (int)rows.size()) : format("%d rows: %s", (int)rows.size(), rows[0].detail.c_str()));
            report("and the other copy is noted on it", !rows.empty() && rows[0].notes == 1, rows.empty() ? "" : format("%d", rows[0].notes));
            person.shot("real_game_with_dlc");
            report("what's noted is in the menu", chooseInMenu(person, app, "About this game") && app.snapshot().dialogText.find("Another copy") != std::string::npos, app.snapshot().dialogText);
            person.press(ButtonA);
        }

        sys::removeTree(games);
        sys::makeDirectories(games);
        copyTo(nsps[2], games + "/base.nsp");
        {
            // What this tool makes of it, as new as it is.
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(games + "/base.nsp"));
            set.group();
            ConvertOptions options;
            std::string made = games + "/" + outputFileName(set.jobs.at(0), set, options);
            NspOutput nsp(made, (int)set.jobs.at(0).contents.size());
            TitleConverter(options, keyset).run(set.jobs.at(0), nsp);
        }
        {
            wipeData(root);
            auto console = freshConsole();
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToTab(person, app, 1);
            person.waitUntilIdle();
            person.settle();
            report("a compressed copy as new takes the original's place", app.snapshot().rows.size() == 1 && app.snapshot().rows[0].compressed && app.snapshot().rows[0].notes == 1);
            chooseInMenu(person, app, "Hide the games that are done");
            person.settle();
            report("and hiding compressed games hides it", app.snapshot().rows.empty() && app.snapshot().compressedHidden == 1,
                   [&] {
                       std::string what = format("%d rows, %d hidden", (int)app.snapshot().rows.size(), app.snapshot().compressedHidden);
                       for (auto& row : app.snapshot().rows)
                           what += format("; %s / %s / compressed %d / %s", row.name.c_str(), row.detail.c_str(), row.compressed ? 1 : 0, row.note.c_str());
                       for (auto& entry : sys::list(games))
                           what += "; file " + entry.name;
                       return what;
                   }());
        }
        // What comes next goes on from the game left there before.
        sys::removeTree(games);
        sys::makeDirectories(games);
        copyTo(nsps[2], games + "/alone.nsp");
        startOver(root);
        sys::removeFile(root + "/data/list.cache");
    }

    // Installing whatever's ticked from the menu, as it is or compressed, from a file or from an extracted game.
    {
        std::string games = root + "/sd/games";
        sys::removeTree(root + "/console4");
        wipeData(root);
        sys::removeTree(root + "/output");
        SimConsole console(root + "/console4", keyset);
        HostPlatform platform(root, keys, &console);
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        goToTab(person, app, 1);
        person.waitUntilIdle();
        person.settle();
        report("a file is there to install", app.snapshot().rows.size() == 1, format("%d", (int)app.snapshot().rows.size()));
        person.press(ButtonPlus);
        person.shot("real_menu_files");
        person.press(ButtonB);
        chooseInMenu(person, app, "Install the ticked");
        report("installing asks first", app.snapshot().dialog == "Install this game?", app.snapshot().dialog);
        person.shot("real_menu_install_confirm");
        person.press(ButtonA);
        person.waitUntilIdle();
        report("installing from the menu finishes", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        person.press(ButtonA);
        std::string problem = console.checkConsistent(keyset);
        report("it's installed, and whole", problem.empty() && console.listApplications().size() == 1, problem);
        report("whatever the options say", readText(root + "/data/settings.txt").find("installgamefiles=1") == std::string::npos);

        // Saved compressed, the NSP takes the original's place on the list, as it's the same game and as new. Installing it takes no compressing.
        goToOption(person, app, "NSP files go in");
        person.press(ButtonA);
        while (!app.snapshot().browsing.empty())
            person.press(ButtonB);
        person.press(ButtonA);
        person.press(ButtonA);
        person.press(ButtonX);
        goToTab(person, app, 1);
        person.waitUntilIdle();
        person.settle();
        person.press(ButtonY);
        if (!app.snapshot().rows.empty() && !app.snapshot().rows[0].ticked)
            person.press(ButtonY);
        person.press(ButtonX);
        person.press(ButtonA);
        person.waitUntilIdle();
        report("saving it finishes", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        person.press(ButtonA);
        person.waitUntilIdle();
        person.settle();
        App::Snapshot both = app.snapshot();
        report("what was made of it takes the original's place on the list", both.rows.size() == 1 && both.rows[0].compressed && both.rows[0].notes == 1,
               both.rows.empty() ? std::string("none") : format("%d rows, compressed %d, %d notes", (int)both.rows.size(), both.rows[0].compressed ? 1 : 0, both.rows[0].notes));
        person.shot("real_files_with_output");

        std::string saved;
        for (auto& entry : sys::list(games)) {
            if (entry.name.find("[LZ4]") != std::string::npos)
                saved = games + "/" + entry.name;
        }
        report("it's where it was asked to go", !saved.empty());
        sys::removeTree(root + "/console4");
        SimConsole again(root + "/console4", keyset);
        (void)again;
    }
    {
        std::string games = root + "/sd/games";
        sys::removeTree(root + "/console5");
        sys::removeFile(root + "/data/list.cache");
        std::string original;
        for (auto& entry : sys::list(games)) {
            if (entry.name.find("[LZ4]") == std::string::npos)
                original = games + "/" + entry.name;
        }
        Bytes keep = readWhole(original);
        sys::removeFile(original);
        SimConsole console(root + "/console5", keyset);
        HostPlatform platform(root, keys, &console);
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        goToTab(person, app, 1);
        person.waitUntilIdle();
        person.settle();
        report("only what was made of it is left", app.snapshot().rows.size() == 1, format("%d", (int)app.snapshot().rows.size()));
        person.press(ButtonY);
        if (!app.snapshot().rows.empty() && !app.snapshot().rows[0].ticked)
            person.press(ButtonY);
        chooseInMenu(person, app, "Install the ticked");
        person.press(ButtonA);
        person.waitUntilIdle();
        report("installing what's already compressed finishes", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        person.press(ButtonA);
        std::string log = readText(root + "/data/log.txt");
        report("without compressing it all over again", log.find("Game data of") == std::string::npos || log.find("Game data of") < log.find("Compressing the game data"), log.substr(0, 200));
        std::string problem = console.checkConsistent(keyset);
        report("and it's whole", problem.empty(), problem);
        sys::File out(original, sys::File::Mode::Create);
        out.write(0, keep.data(), keep.size());
    }

    // An extracted game gets installed the same way.
    {
        sys::removeTree(root + "/console6");
        wipeData(root);
        sys::removeTree(root + "/sd/games");
        sys::removeTree(root + "/output");
        {
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(nsps.size() > 2 ? nsps[2] : nsps[0]));
            set.group();
            extractTitle(set.jobs.at(0), set, root + "/output/extracted");
        }
        SimConsole console(root + "/console6", keyset);
        HostPlatform platform(root, keys, &console);
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        goToTab(person, app, 1);
        person.waitUntilIdle();
        person.settle();
        report("the extracted game is there to install", app.snapshot().rows.size() == 1, format("%d", (int)app.snapshot().rows.size()));
        chooseInMenu(person, app, "Install the ticked");
        report("installing it asks first", app.snapshot().dialog == "Install this game?", app.snapshot().dialog);
        person.press(ButtonA);
        person.waitUntilIdle();
        report("installing an extracted game finishes", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        person.press(ButtonA);
        std::string problem = console.checkConsistent(keyset);
        report("it's on the console, and whole", problem.empty() && console.listApplications().size() == 1, problem);
        auto stray = console.strayFiles();
        report("and nothing's left lying around", stray.empty(), stray.empty() ? "" : stray[0]);
        report("and the folder's still there", !sys::list(root + "/output/extracted").empty());
    }

    // With the checks off, as much as fits stays in memory, and what doesn't only gets read back for its hash.
    for (const char* memory : {"2147483648", "8388608"}) {
        _putenv_s("HBP_TEST_SPARE_MEMORY", memory);
        auto console = freshConsole();
        i64 before = 0;
        for (u64 id : console->listApplications())
            before += console->describe(id).totalBytes();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        goToOption(person, app, "Check what's written");
        report("the checks can be turned off", optionValue(app, "Check what's written") == "On", optionValue(app, "Check what's written"));
        person.press(ButtonRight);
        person.shot("real_options_no_checks");
        goToTab(person, app, 0);
        person.settle();
        person.press(ButtonX);
        report("it doesn't say it checks the copy", app.snapshot().dialogText.find("written and checked") == std::string::npos, app.snapshot().dialogText);
        person.press(ButtonA);
        std::string seen;
        std::map<std::string, bool> labels;
        double start = sys::now();
        while (app.busy() && sys::now() - start < 3600) {
            person.frame();
            std::string label = app.snapshot().label;
            if (!label.empty() && !labels[label]) {
                labels[label] = true;
                seen += label + "; ";
                if (label.find("Working out") == 0)
                    person.shot("real_job_hash");
            }
            sys::sleepMs(2);
        }
        person.settle();
        printf("  with %s bytes to spare: %s\n", memory, seen.c_str());
        report("it finishes", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        report("without comparing anything", seen.find("Checking every byte of the new copy; Checking its other") == std::string::npos && seen.find("Reading the new copy back") == std::string::npos, seen);
        person.press(ButtonA);
        healthy(*console, "with the checks off");
        i64 after = 0;
        for (u64 id : console->listApplications())
            after += console->describe(id).totalBytes();
        report("and it comes out smaller all the same", after < before);
        startOver(root);
    }
    _putenv_s("HBP_TEST_SPARE_MEMORY", "");

    // What's saved is still there the next time, and is said if it can't be.
    {
        auto console = freshConsole();
        std::string folderOnCard = root + "/sd/somewhere";
        sys::makeDirectories(folderOnCard);
        std::map<std::string, std::string> wanted;
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            report("updating by itself is on to begin with", optionValue(app, "Automatic updates") == "On", optionValue(app, "Automatic updates"));
            goToOption(person, app, "Minimum saving");
            person.press(ButtonRight);
            person.press(ButtonRight);
            goToOption(person, app, "Hide games under 1 MB");
            person.press(ButtonRight);
            goToOption(person, app, "Compression level");
            person.press(ButtonLeft);
            goToOption(person, app, "Block size");
            person.press(ButtonLeft);
            goToOption(person, app, "Installed games");
            person.press(ButtonRight);
            goToOption(person, app, "Game files and folders");
            person.press(ButtonRight);
            goToOption(person, app, "Once a file is installed");
            person.press(ButtonRight);
            goToOption(person, app, "Check what's written");
            person.press(ButtonRight);
            goToOption(person, app, "Run the processor faster");
            person.press(ButtonRight);
            goToOption(person, app, "Automatic updates");
            person.press(ButtonRight);
            goToOption(person, app, "NSP files go in");
            person.press(ButtonA);
            while (!app.snapshot().browsing.empty())
                person.press(ButtonB);
            person.press(ButtonA);
            person.press(ButtonA);
            person.press(ButtonX);
            goToTab(person, app, 0);
            person.press(ButtonPlus);
            person.press(ButtonRight);
            person.press(ButtonB);
            goToTab(person, app, SettingsTab);
            for (auto& option : app.snapshot().options)
                wanted[option] = option;
            wanted.erase("Check for updates now: ");
        }
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            goToTab(person, app, SettingsTab);
            App::Snapshot snapshot = app.snapshot();
            int same = 0;
            std::string different;
            for (auto& option : snapshot.options) {
                if (wanted.count(option))
                    same++;
                else if (option.rfind("Check for updates", 0) != 0)
                    different += option + "; ";
            }
            report("every setting is as it was left", different.empty(), different);
            report("and they aren't the usual ones", optionValue(app, "Minimum saving") != "Off" && optionValue(app, "Check what's written") == "Off" && optionValue(app, "Automatic updates") == "Off", optionValue(app, "Minimum saving"));
            report("the order of the list is kept too", app.snapshot().sortedBy == "largest first", app.snapshot().sortedBy);
        }

        // Everything's kept twice, as there's no knowing what could happen to one of them.
        std::string settings = root + "/data/settings.txt";
        std::string answers = root + "/data/analysis.txt";
        report("the settings are kept in a second place", readText(root + "/config/settings.txt") == readText(settings));
        report("and so is what's been worked out", readText(root + "/config/analysis.txt") == readText(answers));
        report("what's kept ends properly", readText(settings).size() > 4 && readText(settings).substr(readText(settings).size() - 4) == "end\n");
        sys::removeFile(settings);
        sys::removeFile(answers);
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            report("the second place is enough for the settings", optionValue(app, "Check what's written") == "Off", optionValue(app, "Check what's written"));
            std::string log = readText(root + "/data/log.txt");
            report("and what's been worked out", log.find("Remembered from last time: 0 answers") == std::string::npos, log.substr(0, 300));
            report("and the log says where they came from", log.find("settings.txt read from " + root + "/config") != std::string::npos, log.substr(0, 500));
            report("and what was in the folders", log.find("In " + root + "/config: ") != std::string::npos, log.substr(0, 500));
        }

        // Half a file: the power went while it was being written.
        {
            std::string whole = readText(root + "/config/settings.txt");
            writeText(settings, whole.substr(0, whole.size() / 2));
            writeText(root + "/config/settings.txt", whole);
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            report("half a file gets passed over for the whole one", optionValue(app, "Check what's written") == "Off" && optionValue(app, "Automatic updates") == "Off", optionValue(app, "Automatic updates"));
        }
        report("there's only the one log", !sys::exists(root + "/data/log.1.txt") && !sys::exists(root + "/data/log.2.txt") && sys::exists(root + "/data/log.txt"));

        // Left waiting beside where it goes, by a power cut at just the wrong moment.
        sys::removeFile(root + "/config/settings.txt");
        sys::renameFile(settings, settings + ".new");
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            report("settings that didn't get their name are still found", optionValue(app, "Check what's written") == "Off", optionValue(app, "Check what's written"));
        }
        sys::removeFile(settings);
        sys::removeFile(settings + ".new");
        sys::removeFile(root + "/config/settings.txt");

        // Something in the way of the file itself, in both places.
        sys::makeDirectories(settings + "/in the way");
        sys::makeDirectories(root + "/config/settings.txt/in the way");
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToOption(person, app, "Minimum saving");
            person.press(ButtonRight);
            person.settle();
            report("not being able to save gets said", app.snapshot().dialog == "That couldn't be saved", app.snapshot().dialog);
            printf("  %s\n", app.snapshot().dialogText.c_str());
            person.shot("real_cant_save");
        }
        sys::removeTree(settings);
        sys::removeTree(root + "/config/settings.txt");
        sys::removeFile(settings + ".new");
    }

    // Asking to compress while another list is still coming up: what's asked for goes first, and the list picks up again afterwards.
    {
        auto console = freshConsole();
        sys::removeTree(root + "/sd/games");
        sys::makeDirectories(root + "/sd/slow/1/2/3/4/5/6");
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            _putenv_s("HBP_TEST_SLOW_LIST_MS", "2000");
            goToTab(person, app, 1);
            person.waitFor([&] { return app.snapshot().scanning; }, 10);
            goToTab(person, app, 0);
            bool anyTicked = false;
            for (auto& row : app.snapshot().rows)
                anyTicked = anyTicked || row.ticked;
            report("with games ticked and game files still coming up", anyTicked && app.snapshot().scanning);
            double started = sys::now();
            person.press(ButtonX);
            person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 60);
            double waited = sys::now() - started;
            report("compressing asks straight away", app.snapshot().dialog.find("Compress") == 0 && waited < 5, format("%s after %.1f s", app.snapshot().dialog.c_str(), waited));
            person.press(ButtonB);
            _putenv_s("HBP_TEST_SLOW_LIST_MS", "");
            person.waitUntilIdle();
            goToTab(person, app, 1);
            person.waitUntilIdle();
            report("and game files come up afterwards", app.snapshot().dialog.empty() && !app.snapshot().scanning, app.snapshot().dialog);
        }
        sys::removeTree(root + "/sd/slow");
    }

    waitingOnPc();

    // The same on the list of game files itself, while it comes up again with what was there last time.
    {
        const std::string& alone = nsps.size() > 2 ? nsps[2] : nsps[0];
        sys::makeDirectories(root + "/sd/games");
        {
            Bytes bytes = readWhole(alone);
            sys::File out(root + "/sd/games/alone.nsp", sys::File::Mode::Create);
            out.write(0, bytes.data(), bytes.size());
        }
        sys::makeDirectories(root + "/sd/slow/1/2/3/4/5/6");
        auto console = freshConsole();
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToTab(person, app, 1);
            person.waitUntilIdle();
        }
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            _putenv_s("HBP_TEST_SLOW_LIST_MS", "2000");
            goToTab(person, app, 1);
            person.waitFor([&] { return app.snapshot().scanning && !app.snapshot().rows.empty(); }, 10);
            bool anyTicked = false;
            for (auto& row : app.snapshot().rows)
                anyTicked = anyTicked || row.ticked;
            report("with a game file ticked and its list still coming up", anyTicked && app.snapshot().scanning, format("%d rows", (int)app.snapshot().rows.size()));
            double started = sys::now();
            person.press(ButtonX);
            person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 60);
            double waited = sys::now() - started;
            std::string dialog = app.snapshot().dialog;
            report("pressing X on it asks straight away", !dialog.empty() && dialog.find("Nothing") == std::string::npos && waited < 5, format("%s after %.1f s", dialog.c_str(), waited));
            person.press(ButtonB);
            _putenv_s("HBP_TEST_SLOW_LIST_MS", "");
            person.waitUntilIdle();
            report("and the list comes up afterwards", app.snapshot().dialog.empty() && !app.snapshot().scanning && !app.snapshot().rows.empty(), app.snapshot().dialog);
        }
        sys::removeTree(root + "/sd/slow");
        sys::removeTree(root + "/sd/games");
    }

    // The buttons: B to quit, L and R all the way round, and sorting in the menu.
    {
        auto console = freshConsole();
        sys::removeTree(root + "/sd/games");
        sys::makeDirectories(root + "/sd/games");
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitFor([&] { return app.snapshot().analyzing; }, 60);
            person.press(ButtonB);
            report("B while it's analyzing asks", app.snapshot().dialog == "Quit hacBrewPackLZ?" && app.snapshot().dialogText.find("analyzing") != std::string::npos, app.snapshot().dialog);
            person.shot("real_quit_question");
            person.press(ButtonB);
            report("and B again says no", !app.wantsToQuit() && app.snapshot().dialog.empty(), app.snapshot().dialog);
            person.waitUntilIdle();
            report("what it was doing got done", !app.snapshot().rows.empty() && app.snapshot().rows[0].hasEstimate);

            // Analyzing again.
            std::string log = readText(root + "/data/log.txt");
            size_t before = 0;
            for (size_t at = log.find("Analyzing "); at != std::string::npos; at = log.find("Analyzing ", at + 1))
                before++;
            chooseInMenu(person, app, "Analyze the ticked games again");
            report("analyzing again forgets the answers", person.waitFor([&] { return app.snapshot().analyzing; }, 20));
            person.waitUntilIdle();
            log = readText(root + "/data/log.txt");
            size_t after = 0;
            for (size_t at = log.find("Analyzing "); at != std::string::npos; at = log.find("Analyzing ", at + 1))
                after++;
            report("and works them out again", after == before * 2, format("%d then %d", (int)before, (int)after));

            // Sorting.
            person.press(ButtonStickR);
            report("the right stick does nothing now", app.snapshot().sortedBy == "best saving first", app.snapshot().sortedBy);
            person.press(ButtonPlus);
            report("+ opens the menu", !app.snapshot().menu.empty());
            person.shot("real_menu");

            // Holding a direction down keeps going.
            {
                InputState held;
                held.held = ButtonDown;
                for (int i = 0; i < 60; i++)
                    person.frame(held);
                person.frame();
                report("holding down goes down the menu", app.snapshot().menuFocus >= 2, format("%d", app.snapshot().menuFocus));
                held.held = ButtonUp;
                for (int i = 0; i < 90; i++)
                    person.frame(held);
                person.frame();
                report("and holding up comes back", app.snapshot().menuFocus == 0, format("%d", app.snapshot().menuFocus));
            }
            report("with the sorting on one row", app.snapshot().menu[0] == "Sort by: Best saving", app.snapshot().menu[0]);
            person.press(ButtonRight);
            report("changed with right", app.snapshot().menu[0] == "Sort by: Size" && app.snapshot().sortedBy == "largest first", app.snapshot().menu[0]);
            person.press(ButtonRight);
            report("and right again", app.snapshot().menu[0] == "Sort by: Name" && app.snapshot().sortedBy == "by name", app.snapshot().menu[0]);
            person.press(ButtonRight);
            report("and round to the start", app.snapshot().menu[0] == "Sort by: Best saving", app.snapshot().menu[0]);
            person.press(ButtonLeft);
            report("and back with left", app.snapshot().menu[0] == "Sort by: Name", app.snapshot().menu[0]);
            person.press(ButtonRight);
            person.press(ButtonB);
            report("B closes the menu without quitting", app.snapshot().menu.empty() && !app.wantsToQuit());

            // Refreshing.
            {
                const std::string& alone = nsps.size() > 2 ? nsps[2] : nsps[0];
                // Extracted games left by what came before are on this list too, so they go first.
                sys::removeTree(root + "/output/extracted");
                goToTab(person, app, 1);
                person.waitUntilIdle();
                chooseInMenu(person, app, "Refresh the list");
                person.waitUntilIdle();
                person.settle();
                person.settle();
                report("nothing's there to begin with", app.snapshot().rows.empty(), format("%d", (int)app.snapshot().rows.size()));
                Bytes bytes = readWhole(alone);
                sys::File out(root + "/sd/games/new one.nsp", sys::File::Mode::Create);
                out.write(0, bytes.data(), bytes.size());
                out.close();
                chooseInMenu(person, app, "Refresh the list");
                person.waitUntilIdle();
                person.settle();
                report("refreshing finds what's new", app.snapshot().rows.size() == 1, format("%d", (int)app.snapshot().rows.size()));
            }

            // All the way round.
            goToTab(person, app, 0);
            person.press(ButtonL);
            report("L from the first list goes round to the settings", app.snapshot().tab == SettingsTab && app.snapshot().screen == "Options", app.snapshot().screen);
            person.shot("real_settings_tab");
            person.press(ButtonR);
            report("and R back round", app.snapshot().tab == 0, format("%d", app.snapshot().tab));
            person.press(ButtonL);
            person.press(ButtonB);
            report("B on the settings asks too", !app.wantsToQuit() && app.snapshot().dialog == "Quit hacBrewPackLZ?", app.snapshot().dialog);
            person.shot("real_quit_settings");
            person.press(ButtonA);
            report("and A goes", app.wantsToQuit());
        }
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            std::string log = readText(root + "/data/log.txt");
            report("the answers are still there next time", log.find("Analyzing") == std::string::npos, log.substr(0, 300));
            report("as the log says", log.find("Remembered from last time") != std::string::npos);
            person.press(ButtonB);
            report("B asks even with nothing going on", !app.wantsToQuit() && app.snapshot().dialog == "Quit hacBrewPackLZ?", app.snapshot().dialog);
            person.press(ButtonB);
            report("and B again stays", !app.wantsToQuit() && app.snapshot().dialog.empty(), app.snapshot().dialog);
        }

        // A game that couldn't be analyzed doesn't get tried again every time, only when it's asked for.
        {
            std::string answers = readText(root + "/data/analysis.txt");
            size_t tab = answers.find('\t');
            size_t end = answers.find('\n', tab);
            writeText(root + "/data/analysis.txt", answers.substr(0, tab + 1) + "failed It didn't work for some reason" + answers.substr(end));
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            std::string log = readText(root + "/data/log.txt");
            report("what couldn't be worked out isn't tried again by itself", log.find("Analyzing") == std::string::npos);
            bool said = false;
            for (auto& row : app.snapshot().rows)
                said = said || row.note == "It didn't work for some reason";
            report("and it says why", said);
            person.shot("real_remembered_failure");
            App::Snapshot failedOne = app.snapshot();
            size_t which = 0;
            while (which < failedOne.rows.size() && failedOne.rows[which].note != "It didn't work for some reason")
                which++;
            report("and it comes up unticked", which < failedOne.rows.size() && !failedOne.rows[which].ticked);
            for (size_t i = 0; i < which; i++)
                person.press(ButtonDown);
            person.press(ButtonA);
            chooseInMenu(person, app, "Analyze the ticked games again");
            person.waitUntilIdle();
            bool allKnown = true;
            for (auto& row : app.snapshot().rows)
                allKnown = allKnown && row.hasEstimate && row.note != "It didn't work for some reason";
            report("until it's asked to", allKnown);
        }
        startOver(root);
        sys::removeFile(root + "/data/list.cache");
        sys::removeTree(root + "/sd/games");
    }

    // The list on the screen comes first.
    {
        sys::makeDirectories(root + "/sd/many");
        for (int i = 0; i < 1500; i++)
            writeText(root + "/sd/many/" + format("nothing %d.nsp", i), "not a game");
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        person.press(ButtonR);
        person.waitFor([&] { return app.snapshot().scanning; }, 5);
        person.press(ButtonL);
        report("going back to a list goes back to it", app.snapshot().tab == 0);
        person.waitUntilIdle();
        std::string log = readText(root + "/data/log.txt");
        report("and the other one carries on being looked through", log.find("Stopped") == std::string::npos && log.find("of them opened") != std::string::npos, log.substr(log.size() > 300 ? log.size() - 300 : 0));
        sys::removeTree(root + "/sd/many");
        sys::removeFile(root + "/data/list.cache");
    }

    // Games that small with nothing else to them are hidden, unless that's turned off.
    {
        auto console = freshConsole();
        sys::removeTree(root + "/output");
        {
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(nsps.size() > 2 ? nsps[2] : nsps[0]));
            set.group();
            extractTitle(set.jobs.at(0), set, root + "/output/extracted");
        }
        std::string game;
        for (auto& entry : sys::list(root + "/output/extracted"))
            game = root + "/output/extracted/" + entry.name;
        sys::removeTree(game + "/romfs");
        i64 bytes = ExtractedGame::open(game).inputBytes;
        if (bytes < (1 << 20)) {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            person.press(ButtonR);
            person.press(ButtonR);
            person.waitUntilIdle();
            report("a game under 1 MB stays out of the list", app.snapshot().rows.empty(), format("%d", (int)app.snapshot().rows.size()));
            goToOption(person, app, "Hide games under 1 MB");
            person.press(ButtonRight);
            goToTab(person, app, 1);
            person.waitUntilIdle();
            person.settle();
            report("until that's turned off", app.snapshot().rows.size() == 1, format("%d", (int)app.snapshot().rows.size()));
        } else {
            printf("  that game is too large to try this with: %lld bytes\n", (long long)bytes);
        }
        sys::removeTree(root + "/output");
        startOver(root);
        sys::removeFile(root + "/data/list.cache");
    }

    // Updating itself.
    if (const char* folder = getenv("UPDATE_DIR")) {
        std::string dir = folder;
        const std::string listing = "https://gitlab.com/api/v4/projects/ismaellozanoperezserrano%2Fhacbrewpacklz/releases?per_page=1";
        const std::string base = "https://gitlab.com/ismaellozanoperezserrano/hacbrewpacklz/-/releases/v9.9.9/downloads/";
        std::string program = root + "/program.bin";
        auto attempt = [&](const std::function<void(HostPlatform&)>& setUp) {
            writeText(program, "the old program");
            auto console = freshConsole();
            HostPlatform platform(root, keys, console.get());
            platform.online = true;
            platform.program = program;
            sys::makeDirectories(root + "/data");
            writeText(root + "/data/settings.txt", "updates=0\nmoresettings=1\nwritelog=1\n");
            std::string text = "[{\"name\":\"hacBrewPackLZ 9.9.9\",\"tag_name\":\"v9.9.9\",\"description\":\"\"}]";
            platform.web[listing] = Bytes(text.begin(), text.end());
            platform.web[base + "hacbrewpacklz-update.txt"] = readWhole(dir + "/hacbrewpacklz-update.txt");
            platform.web[base + "hacbrewpacklz-update.sig"] = readWhole(dir + "/hacbrewpacklz-update.sig");
            platform.web[base + "program.bin"] = readWhole(dir + "/program.bin");
            setUp(platform);
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitUntilIdle();
            goToOption(person, app, "Check for updates now");
            person.press(ButtonA);
            person.waitFor([&] { return !app.snapshot().dialog.empty() || app.snapshot().updateState.find("latest") != std::string::npos; }, 10);
            App::Snapshot result = app.snapshot();
            person.shot("real_update_" + std::to_string(sys::list(shots).size()));
            return result;
        };

        Bytes wanted = readWhole(dir + "/program.bin");
        App::Snapshot done = attempt([](HostPlatform&) {});
        report("a signed release gets installed", readWhole(program) == wanted, done.dialog + " " + done.dialogText);
        report("and says so", done.dialog.find("Updated to version 9.9.9") == 0, done.dialog);

        done = attempt([&](HostPlatform& platform) { platform.web[base + "hacbrewpacklz-update.sig"][20] ^= 1; });
        report("a bad signature keeps the old program", readText(program) == "the old program" && done.dialogText.find("signature") != std::string::npos, done.dialogText);

        done = attempt([&](HostPlatform& platform) { platform.web[base + "hacbrewpacklz-update.txt"][10] ^= 1; });
        report("so does a changed list of what's in the release", readText(program) == "the old program" && done.dialogText.find("signature") != std::string::npos, done.dialogText);

        done = attempt([&](HostPlatform& platform) { platform.web[base + "program.bin"][4] ^= 1; });
        report("so does a damaged download", readText(program) == "the old program" && done.dialogText.find("isn't what the release says") != std::string::npos, done.dialogText);

        done = attempt([&](HostPlatform& platform) {
            std::string older = "[{\"tag_name\":\"v1.0.0\"}]";
            platform.web[listing] = Bytes(older.begin(), older.end());
        });
        report("an older release gets left alone", readText(program) == "the old program" && done.updateState.find("latest") != std::string::npos, done.updateState);

        done = attempt([&](HostPlatform& platform) { platform.online = false; });
        report("not being online gets said", readText(program) == "the old program" && done.dialog == "Not connected", done.dialog);
        startOver(root);

        // Left as it is to begin with, it goes and looks by itself.
        {
            writeText(program, "the old program");
            auto console = freshConsole();
            HostPlatform platform(root, keys, console.get());
            platform.online = true;
            platform.program = program;
            std::string text = "[{\"tag_name\":\"v9.9.9\"}]";
            platform.web[listing] = Bytes(text.begin(), text.end());
            platform.web[base + "hacbrewpacklz-update.txt"] = readWhole(dir + "/hacbrewpacklz-update.txt");
            platform.web[base + "hacbrewpacklz-update.sig"] = readWhole(dir + "/hacbrewpacklz-update.sig");
            platform.web[base + "program.bin"] = readWhole(dir + "/program.bin");
            App app(platform);
            app.start();
            Person person(app, shots);
            person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 30);
            report("it updates itself without being asked", readWhole(program) == wanted && app.snapshot().dialog.find("Updated to version 9.9.9") == 0, app.snapshot().dialog);
        }
        startOver(root);
    }

    // Too old a firmware.
    {
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        platform.newFirmware = false;
        platform.battery = 12;
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        person.press(ButtonX);
        report("too old a firmware gets told so", app.snapshot().dialog.find("too old") != std::string::npos, app.snapshot().dialog);
        person.shot("real_old_firmware");
        person.press(ButtonA);

    }

    // What's been done goes out of the way, and what's next is in front of whoever comes back.
    {
        auto console = freshConsole();
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        App::Snapshot before = app.snapshot();
        report("there are games to choose from", before.rows.size() >= 2, format("%d", (int)before.rows.size()));
        if (before.rows.size() >= 2) {
            std::string done = before.rows[0].name, next = before.rows[1].name;
            person.press(ButtonY);
            if (app.snapshot().tickHint == "Untick all")
                person.press(ButtonY);
            if (app.snapshot().rows[0].ticked == false)
                person.press(ButtonA);
            for (size_t i = 1; i < before.rows.size(); i++) {
                person.press(ButtonDown);
                if (app.snapshot().rows[i].ticked)
                    person.press(ButtonA);
            }
            person.press(ButtonX);
            report("only the first is going", app.snapshot().dialog == "Compress this game?", app.snapshot().dialog);
            person.press(ButtonA);
            person.waitUntilIdle();
            person.press(ButtonA);
            person.waitUntilIdle();
            App::Snapshot after = app.snapshot();
            report("the game that's been done goes to the end of the list", after.rows.back().name == done, after.rows.back().name);
            report("and the next one's at the top", after.rows[0].name == next, after.rows[0].name);
            person.shot("real_done_at_the_end");
        }
    }

    // A game whose base can't be read doesn't count as having gone, whatever its DLC can do.
    {
        auto console = freshConsole();
        for (u64 id : console->listApplications()) {
            InstalledGame game = console->describe(id);
            bool hasDlc = std::any_of(game.metas.begin(), game.metas.end(), [](const InstalledMeta& meta) { return meta.key.type == Cnmt::MetaAddOnContent; });
            if (!hasDlc)
                continue;
            for (auto& meta : game.metas) {
                if (meta.key.type != Cnmt::MetaApplication)
                    continue;
                for (auto& content : meta.contents) {
                    if (content.type == Cnmt::ContentTypeProgram)
                        sys::removeFile(console->contentPath(meta.storage, content.id));
                }
            }
        }
        HostPlatform platform(root, keys, console.get());
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        bool saysWhy = false, bogus = false;
        for (auto& row : app.snapshot().rows) {
            saysWhy = saysWhy || !row.note.empty();
            bogus = bogus || (!row.note.empty() && row.hasEstimate);
        }
        report("a game with its base missing says so", saysWhy);
        bool tickedAnyway = false;
        for (auto& row : app.snapshot().rows)
            tickedAnyway = tickedAnyway || (!row.note.empty() && row.ticked);
        report("and doesn't stay ticked", !tickedAnyway);
        report("rather than claiming it would all go", !bogus);
        person.shot("real_base_missing");
    }

    // The first time it's opened.
    {
        auto console = freshConsole();
        sys::removeTree(root + "/data");
        sys::removeTree(root + "/config");
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            Person person(app, shots);
            person.readsWelcome = true;
            person.settle();
            report("the welcome screen comes first", app.snapshot().screen == "Welcome", app.snapshot().screen);
            person.shot("real_welcome");
            for (auto& item : app.snapshot().welcome)
                printf("  %s\n", item.c_str());
            report("a minimum of 5% to begin with", optionValue(app, "Minimum saving") == "5%", optionValue(app, "Minimum saving"));
            // Checking what's written is under More settings, so it's what the welcome screen says about it that's looked at.
            bool startsFast = app.snapshot().welcome.size() > 3 && app.snapshot().welcome[3] == "Check what's written: No, faster";
            report("no checking to begin with", startsFast, app.snapshot().welcome.size() > 3 ? app.snapshot().welcome[3] : "");
            App::Snapshot first = app.snapshot();
            report("and it starts on what's installed", first.welcome.size() >= 7 && first.welcome[0] == "Installed games: Replace them", first.welcome.empty() ? "" : first.welcome[0]);
            report("game files get installed to begin with", first.welcome.size() >= 7 && first.welcome[1] == "Game files and folders: Install them", first.welcome.size() > 1 ? first.welcome[1] : "");
            report("and updates are automatic", first.welcome.size() >= 7 && first.welcome[5] == "Automatic updates: On", first.welcome.size() > 5 ? first.welcome[5] : "");
            person.press(ButtonRight);
            report("which can be changed", app.snapshot().welcome[0] == "Installed games: Save an NSP" && optionValue(app, "Installed games") == "Save an NSP", app.snapshot().welcome[0]);
            person.press(ButtonLeft);
            person.press(ButtonDown);
            person.press(ButtonDown);
            person.press(ButtonA);
            report("choosing a folder goes to the file explorer", app.snapshot().screen == "Browse", app.snapshot().screen);
            person.press(ButtonMinus);
            report("and back to the welcome screen", app.snapshot().screen == "Welcome", app.snapshot().screen);
            person.press(ButtonDown);
            person.press(ButtonRight);
            report("checking is a choice", app.snapshot().welcome[3] == "Check what's written: Yes, safer", app.snapshot().welcome[3]);
            person.shot("real_welcome_safe");
            person.press(ButtonLeft);
            for (int i = 0; i < 8; i++)
                person.press(ButtonDown);
            person.press(ButtonA);
            report("starting goes to the list", app.snapshot().screen == "Main", app.snapshot().screen);
            person.waitUntilIdle();
            report("what's on the list got looked at in the meantime", !app.snapshot().rows.empty());
            person.press(ButtonX);
            person.press(ButtonB);
            report("and it's remembered", readText(root + "/data/settings.txt").find("welcomed=1") != std::string::npos);
        }
        {
            HostPlatform platform(root, keys, console.get());
            App app(platform);
            app.start();
            report("it only comes up the once", app.snapshot().screen == "Main", app.snapshot().screen);
            Person person(app, shots);
            person.readsWelcome = true;
            person.settle();
            goToOption(person, app, "Welcome screen");
            person.press(ButtonA);
            report("and it can be brought back", app.snapshot().screen == "Welcome", app.snapshot().screen);
            person.press(ButtonPlus);
            report("and + leaves it", app.snapshot().screen == "Options", app.snapshot().screen);
        }
    }

    // A game file that's set to be saved as an NSP, installed from the + menu instead, and the folder chooser along the way.
    {
        sys::removeTree(root + "/console7");
        wipeData(root);
        sys::removeTree(root + "/sd/games");
        sys::makeDirectories(root + "/sd/games/more");
        sys::removeTree(root + "/output");
        const std::string& alone = nsps.size() > 2 ? nsps[2] : nsps[0];
        {
            Bytes bytes = readWhole(alone);
            sys::File out(root + "/sd/games/one to install.nsp", sys::File::Mode::Create);
            out.write(0, bytes.data(), bytes.size());
        }
        writeText(root + "/sd/games/not a game.txt", "nothing to see");
        SimConsole console(root + "/console7", keyset);
        HostPlatform platform(root, keys, &console);
        App app(platform);
        app.start();
        Person person(app, shots);
        person.waitUntilIdle();
        report("nothing's installed to begin with", app.snapshot().rows.empty(), format("%d", (int)app.snapshot().rows.size()));
        person.press(ButtonR);
        person.waitUntilIdle();
        report("the game file is on the list, and nothing else", app.snapshot().rows.size() == 1, format("%d", (int)app.snapshot().rows.size()));

        goToMenuRow(person, app, "Only look in one folder");
        person.press(ButtonA);
        report("the + menu opens a folder chooser", app.snapshot().screen == "Browse", app.snapshot().screen);
        person.press(ButtonMinus);
        report("and - closes it", app.snapshot().screen == "Main", app.snapshot().screen);
        goToMenuRow(person, app, "Only look in one folder");
        person.press(ButtonA);
        person.press(ButtonA);
        App::Snapshot inside = app.snapshot();
        report("in folders", !inside.browseEntries.empty() && inside.browseEntries[0] == "games", inside.browseEntries.empty() ? "" : inside.browseEntries[0]);
        person.press(ButtonA);
        inside = app.snapshot();
        report("with only folders listed", inside.browseEntries.size() == 1 && inside.browseEntries[0] == "more", inside.browseEntries.empty() ? "" : inside.browseEntries.back());
        person.press(ButtonMinus);

        goToMenuRow(person, app, "Install this game");
        person.press(ButtonA);
        bool asked = person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 60);
        report("installing it asks first", asked && app.snapshot().dialog == "Install this game?", app.snapshot().dialog);
        person.shot("real_install_confirm");
        person.press(ButtonB);
        person.settle();
        report("saying no changes nothing", app.snapshot().screen == "Main" && console.listApplications().empty(), app.snapshot().screen);
        goToMenuRow(person, app, "Install this game");
        person.press(ButtonA);
        person.waitFor([&] { return !app.snapshot().dialog.empty(); }, 60);
        person.press(ButtonA);
        person.waitUntilIdle();
        report("and it installs", app.snapshot().resultsTitle == "Finished", app.snapshot().resultsTitle + " " + app.snapshot().resultsDetail);
        person.shot("real_install_results");
        person.press(ButtonA);
        person.press(ButtonL);
        person.waitUntilIdle();
        person.settle();
        report("on the list of what's installed", app.snapshot().tab == 0 && app.snapshot().rows.size() == 1, format("%d", (int)app.snapshot().rows.size()));
        report("and the file is kept, as it was set to be saved", sys::exists(root + "/sd/games/one to install.nsp"));
        std::string problem = console.checkConsistent(keyset);
        report("and it's whole", problem.empty() && console.listApplications().size() == 1, problem);
        auto stray = console.strayFiles();
        report("with nothing left lying around", stray.empty(), stray.empty() ? "" : stray[0]);
        sys::removeTree(root + "/sd/games");
    }

    // Installing from a PC running hacbrewpacklz send, over the network, when there's one to try with: HBP_TEST_PC says where it is, and
    // HBP_TEST_PC_SHARE which folder it's sharing.
    pcInstall();

    printf("%d passed, %d failed\n", g_passes, g_failures);
    return g_failures > 0 ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 1 << 16);
    std::string keys;
    std::vector<std::string> plain;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-k")
            keys = argv[++i];
        else
            plain.push_back(arg);
    }
    try {
        if (plain.size() == 2 && plain[0] == "icon")
            return writeIcon(plain[1]);
        if (plain.size() == 2 && plain[0] == "fonts")
            return runFonts(plain[1]);
        if (plain.size() == 2 && plain[0] == "hashtree")
            return runHashTree(atoi(plain[1].c_str()));
        if (plain.size() < 3) {
            printf("app_test -k keys folder shots demo\napp_test -k keys folder shots real base.nsp update.nsp [other.nsp]\napp_test fonts shots\napp_test icon file.jpg\n");
            return 2;
        }
        sys::makeDirectories(plain[0]);
        sys::makeDirectories(plain[1]);
        if (plain[2] == "demo")
            return runDemo(keys, plain[0], plain[1]);
        return runReal(keys, plain[0], plain[1], std::vector<std::string>(plain.begin() + 3, plain.end()));
    } catch (const std::exception& e) {
        printf("FAIL  %s\n", e.what());
        return 1;
    }
}
