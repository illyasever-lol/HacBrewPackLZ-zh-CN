// What the tests of the app share: the pretend platform, the pretend person who presses the buttons, and ways round the screens.
#pragma once

#include <winsock2.h>
#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <set>

#include "../app/app.h"

namespace apptest {

using namespace hbp;


inline int g_failures = 0;
inline int g_passes = 0;

inline void report(const std::string& name, bool ok, const std::string& detail = "") {
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

inline u32 crc32Of(const u8* data, size_t size, u32 crc = 0) {
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

inline void writePng(const std::string& path, const u32* pixels, int width, int height) {
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

inline bool decodeJpegWithWindows(const Bytes& jpeg, Image& out) {
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

inline Bytes readWhole(const std::string& path) {
    sys::File file(path, sys::File::Mode::Read);
    Bytes data((size_t)file.size());
    if (!data.empty())
        file.read(0, data.data(), data.size());
    return data;
}

inline std::string readText(const std::string& path) {
    if (!sys::exists(path))
        return "";
    Bytes data = readWhole(path);
    return std::string(data.begin(), data.end());
}

inline void writeText(const std::string& path, const std::string& text) {
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

    bool decodeJpeg(const Bytes& jpeg, Image& out) override { return decodeJpegWithWindows(jpeg, out); }
    bool prefersDark() override { return dark; }
    std::string dataFolder() override { return _folder + "/data"; }
    std::string backupFolder() override { return _folder + "/config"; }
    std::string outputFolder() override { return output.empty() ? _folder + "/output" : output; }
    std::vector<Drive> drives() override {
        std::vector<Drive> list{{"SD card", _folder + "/sd", false}};
        if (usb)
            list.push_back(Drive{"USB: Test Drive (exFAT)", _folder + "/usb", true});
        return list;
    }
    bool drivesChanged() override { return usbChanged.exchange(false); }
    std::vector<std::string> foldersToSkip() override { return {_folder + "/sd/Nintendo"}; }
    std::vector<std::string> appFolders() override { return {_folder + "/sd/switch"}; }
    std::vector<std::string> keyLocations() override { return {_keys}; }
    std::vector<std::string> crashReportFolders() override { return {_folder + "/atmosphere/crash_reports", _folder + "/atmosphere/fatal_reports"}; }
    std::vector<GamePlayed> gamesPlayedSince(long long since) override {
        std::vector<GamePlayed> found;
        for (auto& game : played) {
            if (game.started > since)
                found.push_back(game);
        }
        return found;
    }
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
    u32 systemVersion() override { return system; }
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
    std::pair<long, Bytes> post(const std::string& url, const std::string& json, const std::function<bool()>&) override {
        if (!online)
            throw ToolError("Not connected");
        posted.push_back({url, json});
        return {postCode, Bytes(postReply.begin(), postReply.end())};
    }
    std::optional<std::string> typeText(const std::string&, size_t) override {
        keyboardOpened++;
        if (keyboardCancelled)
            return std::nullopt;
        return typed;
    }
    std::string programPath() override { return program; }
    std::string releaseFileName() override { return "program.bin"; }
    bool restartAfterExit(const std::string& path) override {
        restartedAs = path;
        return true;
    }

    bool online = false;
    std::map<std::string, Bytes> web;
    /// What reports were sent where, what the service answers, and what the person types on the keyboard.
    std::vector<std::pair<std::string, std::string>> posted;
    long postCode = 200;
    std::string postReply = "{\"status\":\"received\",\"id\":1}";
    std::string typed;
    /// Where the app's own output goes, when a test wants it somewhere the list of game files looks, as it is on the console's SD card.
    std::string output;
    bool keyboardCancelled = false;
    int keyboardOpened = 0;
    /// What the console's play history says was played, oldest first.
    std::vector<GamePlayed> played;
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
    /// Newer than any game asks for, unless a test says otherwise: 31.0.0.
    u32 system = 31u << 26;

private:
    std::string _folder;
    std::string _keys;
    ConsoleApi* _console;
};


// -------------------------------------------------------------------------------------------------------------------
// The pretend person

class Person {
public:
    Person(App& app, std::string shots) : _app(app), _shots(std::move(shots)), _pixels(1280 * 720) {}

    /// The welcome screen only gets looked at where that's what's being tried.
    bool readsWelcome = false;
    /// Nor the overview the app opens on: R goes from it to the first list, once, as the tests of the lists start there.
    bool looksAtOverview = false;

    void frame(const InputState& input = InputState()) {
        InputState given = input;
        if (!readsWelcome && _app.showingWelcome())
            given.held |= ButtonPlus;
        else if (!looksAtOverview && !_leftOverview && _app.showingOverview() && !(input.held & ButtonR)) {
            given.held |= ButtonR;
            _leftOverview = true;
        }
        _frameMs = -1;
        double updating = sys::now();
        _app.update(given, 1 / 60.0);
        updating = (sys::now() - updating) * 1000;
        if (updating > _worstUpdate) {
            _worstUpdate = updating;
            _worstUpdateScreen = _app.snapshot().screen + (_app.snapshot().menu.empty() ? "" : " + menu") + (_app.snapshot().dialog.empty() ? "" : " + question");
        }
        if (_app.needsDrawing()) {
            Canvas canvas(_pixels.data(), 1280, 720, 1280);
            _app.draw(canvas);
            _frameMs = _app.lastDrawMs();
            if (_app.lastDrawMs() > _worstDraw) {
                _worstDraw = _app.lastDrawMs();
                _worstScreen = _app.snapshot().screen + (_app.snapshot().menu.empty() ? "" : " + menu") + (_app.snapshot().dialog.empty() ? "" : " + question");
            }
        }
    }

    /// With HBP_TEST_FRAME_TIMES, what each frame took to draw after pressing a button, in order, while what it brings up moves in: what
    /// drops frames on the console, several times slower, is a frame here that's a large part of 16.7 ms. Otherwise a plain press.
    void pressTimed(u32 button, const std::string& name) {
        if (!getenv("HBP_TEST_FRAME_TIMES")) {
            press(button);
            return;
        }
        InputState down;
        down.held = button;
        std::string times;
        for (int i = 0; i < 14; i++) {
            frame(i == 0 ? down : InputState());
            times += _frameMs < 0 ? " -" : format(" %.2f", _frameMs);
        }
        printf("FRAMES %s:%s\n", name.c_str(), times.c_str());
        settle(20);
    }

    void settle(int frames = 45) {
        for (int i = 0; i < frames; i++)
            frame();
    }

    /// The results of a job go back with B, as everything does. The tests written while A went back there too press A there, which is taken
    /// as the B they mean; that A does nothing there now is checked on its own, with pressAsIs.
    void press(u32 button) {
        // A question or the menu over the results still takes A.
        App::Snapshot now = _app.snapshot();
        if (button == ButtonA && now.screen == "Results" && now.dialog.empty() && now.menu.empty())
            button = ButtonB;
        pressAsIs(button);
    }

    void pressAsIs(u32 button) {
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

    /// Every screen a test looks at is also looked over for anything that explains something being cut short, and for a game drawn with its
    /// letters though it has an icon.
    void shot(const std::string& name) {
        Canvas canvas(_pixels.data(), 1280, 720, 1280);
        _app.draw(canvas);
        writePng(_shots + "/" + name + ".png", _pixels.data(), 1280, 720);
        auto snapshot = _app.snapshot();
        std::string said;
        for (auto& text : snapshot.cut)
            said += (said.empty() ? "" : " | ") + text;
        report("nothing that explains something is cut short on " + name, snapshot.cut.empty(), said);
        std::string crowded;
        for (auto& text : snapshot.crowded)
            crowded += (crowded.empty() ? "" : " | ") + text;
        report("no sizes run into the bar beside them on " + name, snapshot.crowded.empty(), crowded);
        std::string letters;
        for (auto& game : snapshot.lettersForIcons)
            letters += (letters.empty() ? "" : ", ") + game;
        report("every game with an icon shows it on " + name, snapshot.lettersForIcons.empty(), letters);
        // The slowest frame since the last picture, to find what drops frames on the console, which is several times slower.
        if (getenv("HBP_TEST_FRAME_TIMES")) {
            // And what each frame of it takes once its text is ready, as when something on it moves: the same frame drawn again.
            double total = 0;
            for (int i = 0; i < 20; i++) {
                Canvas again(_pixels.data(), 1280, 720, 1280);
                _app.draw(again);
                total += _app.lastDrawMs();
            }
            printf("FRAME %s: worst %.2f ms (%s), again %.2f ms, update worst %.2f ms (%s)\n", name.c_str(), _worstDraw, _worstScreen.c_str(), total / 20,
                   _worstUpdate, _worstUpdateScreen.c_str());
        }
        _worstDraw = 0;
        _worstScreen.clear();
        _worstUpdate = 0;
        _worstUpdateScreen.clear();
        // What was shortened to fit, said so it can be gone through: names, figures and labels with an ellipsis.
        std::set<std::string> seen;
        for (auto& text : snapshot.fitCut) {
            if (seen.insert(text).second)
                printf("CUT %s: %s\n", name.c_str(), text.c_str());
        }
    }

private:
    App& _app;
    std::string _shots;
    std::vector<u32> _pixels;
    bool _leftOverview = false;
    double _worstDraw = 0, _worstUpdate = 0;
    /// What the last frame took to draw, or -1 if it wasn't drawn.
    double _frameMs = -1;
    std::string _worstScreen, _worstUpdateScreen;
};

inline bool startsWith(const std::string& text, const std::string& start) { return text.compare(0, start.size(), start) == 0; }

inline const App::RowView* rowNamed(const App::Snapshot& snapshot, const std::string& part) {
    for (auto& row : snapshot.rows) {
        if (containsNoCase(row.name, part))
            return &row;
    }
    return nullptr;
}

/// The settings most of these go by, whatever a new copy of the app would start with.
inline void startOver(const std::string& root) {
    sys::removeFile(root + "/config/settings.txt");
    sys::makeDirectories(root + "/data");
    writeText(root + "/data/settings.txt", "welcomed=1\nproblemsasked=1\nmeasureall=0\ndeleteasked=1\ninstallgamefiles=0\nminimumsaving=0\ncheckwritten=1\nhidecompressed=0\nmoresettings=1\nwritelog=1\nfrompcnetwork=0\nfrompccable=0\n");
}

/// So that a list has to be gone through from nothing, which takes long enough to see what else is going on meanwhile.
inline void forgetIndex(const std::string& root) {
    sys::removeFile(root + "/data/files.cache");
    sys::removeFile(root + "/config/files.cache");
}

inline void wipeData(const std::string& root) {
    sys::removeTree(root + "/data");
    sys::removeTree(root + "/config");
    startOver(root);
}

/// L and R, coming back from the settings first if they're open. The overview, first of all, is -1.
inline void goToTab(Person& person, App& app, int tab) {
    if (app.snapshot().screen == "Options")
        person.press(ButtonB);
    for (int i = 0; i < 3 && app.snapshot().tab != tab; i++)
        person.press(tab > app.snapshot().tab ? ButtonR : ButtonL);
}

inline bool chooseInMenu(Person& person, App& app, const std::string& part);

/// The settings, from the + menu on the lists, as they're had on the console. Anywhere else, such as the welcome, it stays where it is.
inline void openSettings(Person& person, App& app) {
    if (app.snapshot().screen == "Main")
        chooseInMenu(person, app, "Settings ...");
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

/// Up and down to an option in the settings, wherever it is in the list.
inline std::string optionValue(App& app, const std::string& label);

inline void goToOption(Person& person, App& app, const std::string& label) {
    openSettings(person, app);
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

inline std::string optionValue(App& app, const std::string& label) {
    for (auto& option : app.snapshot().options) {
        if (option.rfind(label + ": ", 0) == 0)
            return option.substr(label.size() + 2);
    }
    return "(no such option)";
}

/// Opens the + menu and leaves it open, on the row that has that in it.
inline bool goToMenuRow(Person& person, App& app, const std::string& part) {
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
        // Settings opens a screen of its own rather than another page of the menu.
        if (first[(size_t)i].size() < 4 || first[(size_t)i].compare(first[(size_t)i].size() - 4, 4, " ...") != 0 || first[(size_t)i] == "Settings ...")
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

inline bool chooseInMenu(Person& person, App& app, const std::string& part) {
    if (!goToMenuRow(person, app, part)) {
        report("the menu has " + part, false);
        return false;
    }
    person.press(ButtonA);
    return true;
}

}  // namespace apptest
