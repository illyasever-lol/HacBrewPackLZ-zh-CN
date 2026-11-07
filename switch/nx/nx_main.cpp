// The app as it runs on a console: the screen, the buttons, the touch screen and what the system can do for it.
#include <curl/curl.h>
#include <switch.h>
#include <usbhsfs.h>

#include <sys/stat.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <exception>

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

/// How far opening has got, for when it doesn't get as far as its log. It's written as it goes, so that what's in there says where it stopped.
void noteOpening(const char* format, ...) {
    static bool first = true;
    // Only with Keep a log on, which has to be read straight from the settings, as nothing else is going yet.
    static const bool wanted = [] {
        for (const char* path : {"sdmc:/switch/hacBrewPackLZ/settings.txt", "sdmc:/config/hacBrewPackLZ/settings.txt"}) {
            FILE* settings = fopen(path, "r");
            if (!settings)
                continue;
            char line[512];
            bool on = false;
            while (fgets(line, sizeof(line), settings))
                on = on || strncmp(line, "writelog=1", 10) == 0;
            fclose(settings);
            return on;
        }
        return false;
    }();
    if (!wanted) {
        if (first)
            remove("sdmc:/switch/hacBrewPackLZ/opening.txt");
        first = false;
        return;
    }
    if (first)
        mkdir(DataFolder, 0777);
    FILE* file = fopen("sdmc:/switch/hacBrewPackLZ/opening.txt", first ? "w" : "a");
    first = false;
    if (!file)
        return;
    fprintf(file, "[%.2f s] ", (double)armTicksToNs(armGetSystemTick()) / 1e9);
    va_list arguments;
    va_start(arguments, format);
    vfprintf(file, format, arguments);
    va_end(arguments);
    fputc('\n', file);
    fclose(file);
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
        noteOpening("All of that went: USB %d, network %d, settings %d, battery %d, icons %d, running %d %d, fonts %d, games %s", _usb, _nifm, _setsys, _psm, _caps, _pm, _pminfo, _pl, _console ? "是" : _problem.c_str());
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

    std::vector<std::string> foldersToSkip() override { return {"sdmc:/Nintendo", "sdmc:/emuMMC", "sdmc:/atmosphere", "sdmc:/bootloader", "sdmc:/config", "sdmc:/switch/hacBrewPackLZ"}; }
    std::vector<std::string> keyLocations() override { return {"sdmc:/switch/prod.keys", "sdmc:/prod.keys"}; }

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

    std::string aboutSystem() override {
        u64 heap = 0;
        svcGetInfo(&heap, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        return format("Firmware %d.%d.%d, Atmosphere %s, applet type %d, %d MB of memory, %d cores to work on, USB drives %s, from %s", HOSVER_MAJOR(hosversionGet()), HOSVER_MINOR(hosversionGet()), HOSVER_MICRO(hosversionGet()), hosversionIsAtmosphere() ? "是" : "否", (int)appletGetAppletType(), (int)(heap >> 20), sys::workerCount(), _usb ? "可用" : "不可用", _program.empty() ? "位置未知" : _program.c_str());
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
    NxPlatform platform(std::move(program));
    noteOpening("本程序");
    App app(platform);
    app.start();
    noteFolder("在文件夹中，刚打开");
    noteOpening("屏幕");

    PadState pad;
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();

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
    noteOpening("打开");

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

/// One request at a time. Everything here goes through Atmosphere's fs.mitm, whose threads abort, and take the console with them, if replying comes back
/// as cancelled, which is what one of them waiting can get when another finishes. So the fewer of its threads are busy at once the better. This doesn't
/// cost much, as the system's file service only runs on one core anyway, and threads just wait their turn. 8 sessions froze the console as this opened.
extern "C" u32 __nx_fs_num_sessions;
u32 __nx_fs_num_sessions = 1;

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

    noteOpening("正在退出");
    appletUnlockExit();
    return 0;
}
