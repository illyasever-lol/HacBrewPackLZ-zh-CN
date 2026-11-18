// The app as it runs on a console: the screen, the buttons, the touch screen and what the system can do for it.
#include <curl/curl.h>
#include <switch.h>
#include <usbhsfs.h>

#include <sys/stat.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <exception>
#include <map>

#include "../app/app.h"
#include "../core/remote.h"
#include "nx_console.h"

using namespace hbp;

namespace hbp {
bool startSockets();
void stopSockets();
}

namespace {

constexpr const char* DataFolder = "sdmc:/switch/hacBrewPackLZ";
constexpr const char* BackupFolder = "sdmc:/config/hacBrewPackLZ";
constexpr const char* OutputFolder = "sdmc:/hacBrewPackLZ";

FILE* g_openingFile = nullptr;
double g_openingClosedAt = 0;

/// Closed, so that what's in it is on the card for certain. Done at most once a second as it goes, as each time costs the card a change to its
/// folder, and that was most of the time opening took when every line was written that way.
void closeOpeningNotes() {
    if (g_openingFile)
        fclose(g_openingFile);
    g_openingFile = nullptr;
    g_openingClosedAt = (double)armTicksToNs(armGetSystemTick()) / 1e9;
}

/// How far opening has got, for when it doesn't get as far as its log. It's written as it goes, so that what's in there says where it stopped.
void noteOpening(const char* format, ...) {
    static bool first = true;
    // Only with Keep a log on, as it is to begin with, which has to be read straight from the settings, as nothing else is going yet.
    static const bool wanted = [] {
        for (const char* path : {"sdmc:/switch/hacBrewPackLZ/settings.txt", "sdmc:/config/hacBrewPackLZ/settings.txt"}) {
            FILE* settings = fopen(path, "r");
            if (!settings)
                continue;
            char line[512];
            bool off = false;
            while (fgets(line, sizeof(line), settings))
                off = off || strncmp(line, "writelog=0", 10) == 0;
            fclose(settings);
            return !off;
        }
        return true;
    }();
    if (!wanted) {
        if (first)
            remove("sdmc:/switch/hacBrewPackLZ/opening.txt");
        first = false;
        return;
    }
    if (first)
        mkdir(DataFolder, 0777);
    if (!g_openingFile)
        g_openingFile = fopen("sdmc:/switch/hacBrewPackLZ/opening.txt", first ? "w" : "a");
    first = false;
    if (!g_openingFile)
        return;
    double now = (double)armTicksToNs(armGetSystemTick()) / 1e9;
    fprintf(g_openingFile, "[%.2f s] ", now);
    va_list arguments;
    va_start(arguments, format);
    vfprintf(g_openingFile, format, arguments);
    va_end(arguments);
    fputc('\n', g_openingFile);
    fflush(g_openingFile);
    if (now - g_openingClosedAt >= 1)
        closeOpeningNotes();
}

/// What's in this app's folder as it sees it, for when something doesn't turn up where it should.
void noteFolder(const char* when) {
    std::string files;
    for (auto& entry : sys::list(DataFolder))
        files += format("%s (%lld)  ", entry.name.c_str(), (long long)entry.size);
    noteOpening("%s: %s", when, files.c_str());
}

class NxPlatform : public Platform {
public:
    explicit NxPlatform(std::string program) : _program(std::move(program)) {
        noteOpening("USB 驱动器");
        _usb = R_SUCCEEDED(usbHsFsInitialize(0));
        noteOpening("网络");
        _nifm = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
        noteOpening("系统设置");
        _setsys = R_SUCCEEDED(setsysInitialize());
        noteOpening("电池");
        _psm = R_SUCCEEDED(psmInitialize());
        noteOpening("图标解码器");
        _caps = R_SUCCEEDED(capsdcInitialize());
        noteOpening("正在运行的内容");
        _pm = R_SUCCEEDED(pmdmntInitialize());
        _pminfo = R_SUCCEEDED(pminfoInitialize());
        noteOpening("字体");
        _pl = R_SUCCEEDED(plInitialize(PlServiceType_User));
        noteOpening("主机上的游戏");
        _console = openConsole(_problem);
        noteOpening("All of that went: USB %d, network %d, settings %d, battery %d, icons %d, running %d %d, fonts %d, games %s", _usb, _nifm, _setsys, _psm, _caps, _pm, _pminfo, _pl, _console ? "yes" : _problem.c_str());
    }

    ~NxPlatform() override {
        // Each step of closing is noted, as one that holds it up keeps the home menu away.
        noteOpening("正在关闭主机游戏服务");
        _console.reset();
        noteOpening("正在关闭网络");
        if (_sockets)
            curl_global_cleanup();
        // Only once nothing's using it: the threads looking for PCs have stopped by now.
        hbp::stopSockets();
        if (_nifm)
            nifmExit();
        noteOpening("正在关闭 USB 盘");
        if (_usb)
            usbHsFsExit();
        noteOpening("正在关闭其余服务");
        if (_pl)
            plExit();
        if (_pminfo)
            pminfoExit();
        if (_pm)
            pmdmntExit();
        if (_caps)
            capsdcExit();
        if (_psm)
            psmExit();
        if (_setsys)
            setsysExit();
    }

    ConsoleApi* console() override { return _console.get(); }
    std::string consoleProblem() override { return _problem; }

    void loadFonts(Font& font) override {
        if (!_pl)
            throw ToolError("无法访问系统字体");
        // The one for most languages goes first, and the others are for the characters it hasn't got.
        for (PlSharedFontType type : {PlSharedFontType_Standard, PlSharedFontType_ChineseSimplified, PlSharedFontType_ExtChineseSimplified, PlSharedFontType_ChineseTraditional, PlSharedFontType_KO}) {
            PlFontData data;
            if (R_SUCCEEDED(plGetSharedFontByType(&data, type)))
                font.addFace((const u8*)data.address, data.size);
        }
        if (!font.hasFaces())
            throw ToolError("无法读取系统字体");
    }

    bool decodeIcon(const Bytes& jpeg, Image& out) override {
        constexpr int Size = 256;
        if (!_caps)
            return false;
        sys::giveWay();
        out.w = out.h = Size;
        out.pixels.resize((size_t)Size * Size);
        CapsScreenShotDecodeOption options = {};
        if (R_FAILED(capsdcDecodeJpeg(Size, Size, &options, jpeg.data(), jpeg.size(), out.pixels.data(), out.pixels.size() * 4)))
            return false;
        for (auto& pixel : out.pixels)
            pixel |= 0xFF000000u;
        return true;
    }

    bool prefersDark() override {
        ColorSetId id = ColorSetId_Dark;
        if (_setsys)
            setsysGetColorSetId(&id);
        return id == ColorSetId_Dark;
    }

    std::string dataFolder() override { return DataFolder; }
    std::string backupFolder() override { return BackupFolder; }
    std::string outputFolder() override { return OutputFolder; }

    std::vector<Drive> drives() override {
        std::vector<Drive> list{{"SD 卡", "sdmc:/", false}};
        if (!_usb)
            return list;
        u32 count = usbHsFsGetMountedDeviceCount();
        if (count == 0)
            return list;
        std::vector<UsbHsFsDevice> devices(count);
        count = usbHsFsListMountedDevices(devices.data(), count);
        for (u32 i = 0; i < count; i++) {
            const UsbHsFsDevice& device = devices[i];
            std::string label = device.product_name[0] ? device.product_name : device.name;
            list.push_back(Drive{format("USB: %s (%s)", label.c_str(), LIBUSBHSFS_FS_TYPE_STR(device.fs_type)), std::string(device.name) + "/", true});
        }
        return list;
    }

    bool drivesChanged() override {
        if (!_usb)
            return false;
        UEvent* event = usbHsFsGetStatusChangeUserEvent();
        return event && R_SUCCEEDED(waitSingle(waiterForUEvent(event), 0));
    }

    std::vector<std::string> foldersToSkip() override {
        // SaltySD holds plugins and their patches, a folder for every game's every version with FPSLocker.
        return {"sdmc:/Nintendo", "sdmc:/emuMMC", "sdmc:/atmosphere", "sdmc:/bootloader", "sdmc:/config", "sdmc:/SaltySD", "sdmc:/switch/hacBrewPackLZ"};
    }
    std::vector<std::string> appFolders() override { return {"sdmc:/switch"}; }
    std::vector<std::string> keyLocations() override { return {"sdmc:/switch/prod.keys", "sdmc:/prod.keys"}; }
    std::vector<std::string> crashReportFolders() override { return {"sdmc:/atmosphere/crash_reports", "sdmc:/atmosphere/fatal_reports"}; }

    std::vector<GamePlayed> gamesPlayedSince(long long since) override {
        // Opened only for this, as nothing else needs it. If it can't be, there's nothing to say.
        std::vector<GamePlayed> played;
        if (R_FAILED(pdmqryInitialize()))
            return played;
        s32 total = 0, first = 0, last = 0;
        if (R_SUCCEEDED(pdmqryGetAvailablePlayEventRange(&total, &first, &last)) && total > 0) {
            // The newest few thousand events, in order, a batch at a time, which is enough unless the app was last open long ago. Each has its
            // time by the clock the console shows, which is time()'s, and by a steady clock that setting the time doesn't change.
            constexpr s32 Batch = 256, Most = 4096;
            std::vector<PdmPlayEvent> events(Batch);
            // A game that started, by program, until it ends: where it is in what's played, and when it started by the steady clock.
            std::map<u64, std::pair<size_t, u64>> running;
            s32 next = std::max(first, last - Most);
            for (s32 read = 0; read <= Most;) {
                s32 count = 0;
                if (R_FAILED(pdmqryQueryPlayEvent(next, events.data(), Batch, &count)) || count <= 0)
                    break;
                for (s32 i = 0; i < std::min(count, Batch); i++) {
                    const PdmPlayEvent& event = events[(size_t)i];
                    // Only games: not the system's own applets, nor users, sleep or the dock.
                    if (event.play_event_type != PdmPlayEventType_Applet || event.event_data.applet.applet_id != AppletId_application)
                        continue;
                    // The title ID is kept with its two halves the other way round.
                    u64 program = (u64)event.event_data.applet.program_id[0] << 32 | event.event_data.applet.program_id[1];
                    u8 type = event.event_data.applet.event_type;
                    if (type == PdmAppletEventType_Launch) {
                        running.erase(program);
                        if ((long long)event.timestamp_user > since) {
                            running[program] = {played.size(), event.timestamp_steady};
                            played.push_back(GamePlayed{program, (long long)event.timestamp_user, 0});
                        }
                    } else if (type == PdmAppletEventType_Exit || type == PdmAppletEventType_Exit5 || type == PdmAppletEventType_Exit6) {
                        auto found = running.find(program);
                        if (found == running.end())
                            continue;
                        GamePlayed& game = played[found->second.first];
                        u64 steadyStart = found->second.second;
                        if (steadyStart > 0 && event.timestamp_steady >= steadyStart)
                            game.ended = game.started + (long long)(event.timestamp_steady - steadyStart);
                        else if ((long long)event.timestamp_user >= game.started)
                            game.ended = (long long)event.timestamp_user;
                        running.erase(found);
                    }
                }
                next += count;
                read += count;
                if (count < Batch)
                    break;
            }
        }
        pdmqryExit();
        return played;
    }

    void setWorking(bool working, bool boost) override {
        appletSetAutoSleepDisabled(working);
        appletSetMediaPlaybackState(working);
        appletSetCpuBoostMode(boost ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
    }

    int batteryPercent() override {
        u32 percent = 0;
        return _psm && R_SUCCEEDED(psmGetBatteryChargePercentage(&percent)) ? (int)percent : -1;
    }

    bool isCharging() override {
        PsmChargerType type = PsmChargerType_Unconnected;
        return _psm && R_SUCCEEDED(psmGetChargerType(&type)) && type != PsmChargerType_Unconnected;
    }

    bool memoryIsLimited() override {
        AppletType type = appletGetAppletType();
        return type != AppletType_Application && type != AppletType_SystemApplication;
    }

    bool firmwareReadsCompression() override { return hosversionAtLeast(12, 0, 0); }

    bool decodeJpeg(const Bytes& jpeg, Image& out) override {
        int width = 0, height = 0;
        if (!_caps || !jpegSize(jpeg, width, height) || (i64)width * height > 4096LL * 4096)
            return false;
        sys::giveWay();
        out.w = width;
        out.h = height;
        out.pixels.resize((size_t)width * height);
        CapsScreenShotDecodeOption options = {};
        if (R_FAILED(capsdcDecodeJpeg((u32)width, (u32)height, &options, jpeg.data(), jpeg.size(), out.pixels.data(), out.pixels.size() * 4)))
            return false;
        for (auto& pixel : out.pixels)
            pixel |= 0xFF000000u;
        return true;
    }

    u32 systemVersion() override {
        u32 version = hosversionGet();
        return (u32)HOSVER_MAJOR(version) << 26 | (u32)HOSVER_MINOR(version) << 20 | (u32)HOSVER_MICRO(version) << 16;
    }

    std::string aboutSystem() override {
        u64 heap = 0;
        svcGetInfo(&heap, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        return format("Firmware %d.%d.%d, Atmosphere %s, applet type %d, %d MB of memory, %d cores to work on, USB drives %s, from %s", HOSVER_MAJOR(hosversionGet()), HOSVER_MINOR(hosversionGet()), HOSVER_MICRO(hosversionGet()), hosversionIsAtmosphere() ? "yes" : "no", (int)appletGetAppletType(), (int)(heap >> 20), sys::workerCount(), _usb ? "available" : "不可用", _program.empty() ? "位置未知" : _program.c_str());
    }

    bool isOnline() override {
        NifmInternetConnectionType type;
        u32 strength = 0;
        NifmInternetConnectionStatus status = NifmInternetConnectionStatus_ConnectingUnknown1;
        return _nifm && R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &status)) && status == NifmInternetConnectionStatus_Connected;
    }

    Bytes download(const std::string& url, size_t limit, const std::function<bool()>& keepGoing) override {
        if (!_sockets) {
            if (!hbp::startSockets())
                throw ToolError("网络无法启动");
            curl_global_init(CURL_GLOBAL_DEFAULT);
            _sockets = true;
        }

        struct Receiving {
            Bytes data;
            size_t limit;
            const std::function<bool()>* keepGoing;
        } receiving{{}, limit, &keepGoing};
        curl_write_callback onData = [](char* bytes, size_t size, size_t count, void* user) -> size_t {
            auto* into = (Receiving*)user;
            size_t total = size * count;
            if (into->data.size() + total > into->limit)
                return 0;
            into->data.insert(into->data.end(), (u8*)bytes, (u8*)bytes + total);
            return total;
        };
        curl_xferinfo_callback onProgress = [](void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int { return (*((Receiving*)user)->keepGoing)() ? 0 : 1; };

        CURL* curl = curl_easy_init();
        if (!curl)
            throw ToolError("网络无法启动");
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "hacBrewPackLZ");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onData);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &receiving);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, onProgress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &receiving);
        CURLcode result = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        if (result != CURLE_OK)
            throw ToolError(format("Couldn't get that from GitLab: %s", curl_easy_strerror(result)));
        return std::move(receiving.data);
    }

    std::pair<long, Bytes> post(const std::string& url, const std::string& json, const std::function<bool()>& keepGoing) override {
        if (!hbp::startSockets())
            throw ToolError("网络不可用");
        struct Receiving {
            Bytes data;
            const std::function<bool()>* keepGoing;
        } receiving{{}, &keepGoing};
        curl_write_callback onData = [](char* bytes, size_t size, size_t count, void* user) -> size_t {
            auto* into = (Receiving*)user;
            size_t total = size * count;
            if (into->data.size() + total > (1 << 20))
                return 0;
            into->data.insert(into->data.end(), (u8*)bytes, (u8*)bytes + total);
            return total;
        };
        CURL* curl = curl_easy_init();
        if (!curl)
            throw ToolError("网络无法启动");
        // Given up on when the app closes, rather than holding it open.
        curl_xferinfo_callback onProgress = [](void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int { return (*((Receiving*)user)->keepGoing)() ? 0 : 1; };
        curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "hacBrewPackLZ");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)json.size());
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onData);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &receiving);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, onProgress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &receiving);
        CURLcode result = curl_easy_perform(curl);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        if (result != CURLE_OK)
            throw ToolError(format("Couldn't reach the report service: %s", curl_easy_strerror(result)));
        return {code, std::move(receiving.data)};
    }

    std::optional<std::string> typeText(const std::string& guide, size_t most) override {
        SwkbdConfig keyboard;
        if (R_FAILED(swkbdCreate(&keyboard, 0)))
            return std::string();
        swkbdConfigMakePresetDefault(&keyboard);
        swkbdConfigSetGuideText(&keyboard, guide.c_str());
        swkbdConfigSetStringLenMax(&keyboard, (u32)most);
        std::vector<char> typed(most * 4 + 1, 0);
        Result rc = swkbdShow(&keyboard, typed.data(), typed.size());
        swkbdClose(&keyboard);
        // That's what it says when it's been cancelled.
        if (rc == MAKERESULT(Module_Libnx, LibnxError_LibAppletBadExit))
            return std::nullopt;
        return R_SUCCEEDED(rc) ? std::string(typed.data()) : std::string();
    }

    std::string programPath() override { return _program; }
    std::string releaseFileName() override { return "hacBrewPackLZ.nro"; }

    bool restartAfterExit(const std::string& path) override {
        if (!envHasNextLoad())
            return false;
        std::string arguments = "\"" + path + "\"";
        return R_SUCCEEDED(envSetNextLoad(path.c_str(), arguments.c_str()));
    }

private:
    std::string _program;
    bool _usb = false;
    bool _nifm = false;
    bool _sockets = false;
    std::unique_ptr<ConsoleApi> _console;
    std::string _problem;
    bool _setsys = false;
    bool _psm = false;
    bool _caps = false;
    bool _pm = false;
    bool _pminfo = false;
    bool _pl = false;
};

u32 buttonsOf(u64 held) {
    u32 buttons = 0;
    auto map = [&](u64 from, u32 to) {
        if (held & from)
            buttons |= to;
    };
    map(HidNpadButton_A, ButtonA);
    map(HidNpadButton_B, ButtonB);
    map(HidNpadButton_X, ButtonX);
    map(HidNpadButton_Y, ButtonY);
    map(HidNpadButton_L, ButtonL);
    map(HidNpadButton_R, ButtonR);
    map(HidNpadButton_ZL, ButtonZL);
    map(HidNpadButton_ZR, ButtonZR);
    map(HidNpadButton_Plus, ButtonPlus);
    map(HidNpadButton_Minus, ButtonMinus);
    map(HidNpadButton_AnyUp, ButtonUp);
    map(HidNpadButton_AnyDown, ButtonDown);
    map(HidNpadButton_AnyLeft, ButtonLeft);
    map(HidNpadButton_AnyRight, ButtonRight);
    map(HidNpadButton_StickL, ButtonStickL);
    map(HidNpadButton_StickR, ButtonStickR);
    return buttons;
}

/// When there's nothing to draw with. It's as plain as it gets, as whatever went wrong may be with the rest.
void showFatal(const std::string& message) {
    noteOpening("Stopped: %s", message.c_str());
    closeOpeningNotes();
    consoleInit(nullptr);
    printf("\n  hacBrewPackLZ can't start.\n\n  %s\n\n  Press + to leave.\n", message.c_str());
    consoleUpdate(nullptr);

    PadState pad;
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        svcSleepThread(16000000);
    }
    consoleExit(nullptr);
}

void run(std::string program) {
    // The screen comes first, so that the app can show it's opening as soon as it has a font, rather than leaving it black until it's all read.
    noteOpening("屏幕");
    Framebuffer framebuffer;
    if (R_FAILED(framebufferCreate(&framebuffer, nwindowGetDefault(), 1280, 720, PIXEL_FORMAT_RGBA_8888, 2)))
        throw ToolError("无法在屏幕上绘制");
    framebufferMakeLinear(&framebuffer);
    struct CloseScreen {
        Framebuffer& screen;
        bool open = true;
        ~CloseScreen() {
            if (open)
                framebufferClose(&screen);
        }
    } closeScreen{framebuffer};

    NxPlatform platform(std::move(program));
    noteOpening("本程序");
    App app(platform);
    app.whenItCanBeDrawn = [&] {
        u32 stride = 0;
        u32* pixels = (u32*)framebufferBegin(&framebuffer, &stride);
        Canvas canvas(pixels, 1280, 720, (int)(stride / sizeof(u32)));
        app.drawOpening(canvas);
        framebufferEnd(&framebuffer);
        noteOpening("Showing that it's opening");
    };
    app.start();
    noteFolder("在文件夹中，刚打开");

    PadState pad;
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();
    noteOpening("打开");
    closeOpeningNotes();

    u64 last = armGetSystemTick();
    while (appletMainLoop()) {
        padUpdate(&pad);
        InputState input;
        input.held = buttonsOf(padGetButtons(&pad));
        HidTouchScreenState touch = {};
        if (hidGetTouchScreenStates(&touch, 1) && touch.count > 0) {
            input.touching = true;
            input.touchX = (int)touch.touches[0].x;
            input.touchY = (int)touch.touches[0].y;
        }

        u64 tick = armGetSystemTick();
        double dt = (double)armTicksToNs(tick - last) / 1e9;
        last = tick;
        app.update(input, dt);
        if (app.wantsToQuit())
            break;

        // Out of focus nothing shows, and the work goes on just the same, except for what's on the core the home menu needs.
        bool inFocus = appletGetFocusState() == AppletFocusState_InFocus;
        sys::setInForeground(inFocus);
        if (inFocus && app.needsDrawing()) {
            u32 stride = 0;
            u32* pixels = (u32*)framebufferBegin(&framebuffer, &stride);
            Canvas canvas(pixels, 1280, 720, (int)(stride / sizeof(u32)));
            app.draw(canvas);
            framebufferEnd(&framebuffer);
        } else {
            svcSleepThread(16000000);
        }
    }

    // Anything running stops at the next safe point, which needn't be held up now.
    sys::setInForeground(true);
    app.shutdown();
    noteOpening("正在停止搜索 PC");
    remote::stopLooking();
    noteFolder("在文件夹中，正在关闭");
    closeScreen.open = false;
    framebufferClose(&framebuffer);
}

}  // namespace

/// Three requests to the card at once, libnx's own number, as every homebrew app has. It was one from 2 to 11 November: requests the kernel
/// turned down (2001-0106) can leave Atmosphere's fs.mitm waiting on a session until a stray cancel aborts it and the console with it, and more
/// sessions made that more likely. The requests were turned down for path buffers shorter than the 0x301 bytes libnx sends, which are full-size
/// since (nx_path.h), and four console runs had none turned down. With one, reading a game and writing its copy took turns, and so did everything
/// the app does beside a job. 8 sessions froze a console as this opened, so it stays at the usual three.
extern "C" u32 __nx_fs_num_sessions;
u32 __nx_fs_num_sessions = 3;

int main(int argc, char** argv) {
    u64 coreMask = 0, priorities = 0;
    svcGetInfo(&coreMask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&priorities, InfoType_PriorityMask, CUR_PROCESS_HANDLE, 0);
    noteOpening("hacBrewPackLZ %s, applet type %d, cores %llx, priorities %llx, from %s", AppVersion, (int)appletGetAppletType(), (unsigned long long)coreMask, (unsigned long long)priorities, argc > 0 && argv[0] ? argv[0] : "位置未知");

    // Closing the app from the home menu gets told about, so a job can stop at a safe point rather than just being cut off.
    appletLockExit();
    if (appletGetAppletType() == AppletType_Application || appletGetAppletType() == AppletType_SystemApplication)
        appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend);
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);

    try {
        // What the program is, if whatever opened it says, and it was opened from the SD card.
        std::string program = argc > 0 && argv[0] ? argv[0] : "";
        if (program.compare(0, 6, "sdmc:/") != 0)
            program.clear();
        run(std::move(program));
    } catch (const std::exception& e) {
        showFatal(e.what());
    }

    noteOpening("Leaving");
    closeOpeningNotes();
    appletUnlockExit();
    return 0;
}
