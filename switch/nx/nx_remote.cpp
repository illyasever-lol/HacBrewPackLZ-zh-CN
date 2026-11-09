// How the console gets to a PC: over the network, which it finds by listening for what `hacbrewpacklz send` announces, or over a USB cable.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <switch.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <map>

#include "../core/remote.h"

namespace hbp {

sys::Mutex g_socketsMutex;
bool g_socketsTried = false, g_socketsStarted = false;

/// The network for anything that needs it, started the once.
bool startSockets() {
    sys::Lock lock(g_socketsMutex);
    if (!g_socketsTried) {
        g_socketsTried = true;
        // Larger receive buffers than usual, which a game coming in from a PC needs to keep the network busy. Should the system not have
        // the room for them, the usual ones do too, only slower.
        SocketInitConfig config = *socketGetDefaultInitConfig();
        config.tcp_rx_buf_size = 0x40000;
        config.tcp_rx_buf_max_size = 0x200000;
        g_socketsStarted = R_SUCCEEDED(socketInitialize(&config)) || R_SUCCEEDED(socketInitializeDefault());
    }
    return g_socketsStarted;
}

/// The network keeps some of the app's memory for itself until it's closed, and the homebrew loader can't start another program while it
/// has, as Restart now after updating needs it to.
void stopSockets() {
    sys::Lock lock(g_socketsMutex);
    if (g_socketsStarted)
        socketExit();
    g_socketsStarted = false;
}

namespace remote {

namespace {

constexpr int SecondsToWait = 20;
/// A PC that hasn't been heard from for this long has gone.
constexpr double Forgotten = 6;

class TcpTransport : public Transport {
public:
    TcpTransport(u32 address, u16 port) {
        _socket = socket(AF_INET, SOCK_STREAM, 0);
        if (_socket < 0)
            throw ToolError("网络不可用");
        timeval wait{SecondsToWait, 0};
        setsockopt(_socket, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
        setsockopt(_socket, SOL_SOCKET, SO_SNDTIMEO, &wait, sizeof(wait));
        int on = 1;
        setsockopt(_socket, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
        sockaddr_in to = {};
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        to.sin_addr.s_addr = address;
        if (connect(_socket, (sockaddr*)&to, sizeof(to)) != 0) {
            close(_socket);
            throw ToolError("无法通过网络连接到 PC");
        }
    }
    ~TcpTransport() override { close(_socket); }

    void send(const void* data, size_t size) override {
        const char* in = (const char*)data;
        while (size > 0) {
            waitFor(POLLOUT);
            ssize_t sent = ::send(_socket, in, size, 0);
            if (sent <= 0)
                throw ToolError("PC 已停止通过网络应答");
            in += sent;
            size -= (size_t)sent;
        }
    }

    void receive(void* data, size_t size) override {
        char* out = (char*)data;
        while (size > 0) {
            waitFor(POLLIN);
            ssize_t got = recv(_socket, out, size, 0);
            if (got <= 0)
                throw ToolError("PC 已停止通过网络应答");
            out += got;
            size -= (size_t)got;
        }
    }

private:
    /// Until the socket's ready, a little at a time, so that whatever's waiting can give way when it's asked to.
    void waitFor(short events) {
        while (true) {
            if (shouldGiveWay())
                throw Cancelled();
            pollfd ready = {_socket, events, 0};
            int n = poll(&ready, 1, 100);
            if (n > 0)
                return;
            if (n < 0 && errno != EINTR)
                throw ToolError("PC 已停止通过网络应答");
        }
    }

    int _socket = -1;
};

struct Heard {
    u32 address;
    u16 port;
    std::string name;
    double at;
};

sys::Mutex g_heardMutex;
std::map<std::string, Heard> g_heard;
std::atomic<bool> g_changed{false};
/// The app is closing, and the threads that listen for PCs and watch the cable stop, before the network and the USB port go.
std::atomic<bool> g_stopping{false};
sys::Thread* g_listener = nullptr;
sys::Thread* g_cableWatcher = nullptr;

/// What the PCs on the network announce, every second.
void listenForPcs() {
    int listening = socket(AF_INET, SOCK_DGRAM, 0);
    if (listening < 0)
        return;
    int on = 1;
    setsockopt(listening, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    timeval wait{1, 0};
    setsockopt(listening, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
    sockaddr_in here = {};
    here.sin_family = AF_INET;
    here.sin_port = htons(BeaconPort);
    here.sin_addr.s_addr = INADDR_ANY;
    if (bind(listening, (sockaddr*)&here, sizeof(here)) != 0) {
        close(listening);
        return;
    }
    char buffer[256];
    while (!g_stopping) {
        sockaddr_in from = {};
        socklen_t fromSize = sizeof(from);
        ssize_t got = recvfrom(listening, buffer, sizeof(buffer) - 1, 0, (sockaddr*)&from, &fromSize);
        // Waking from sleep can leave the network not there for a while, when this would otherwise go round as fast as it could.
        if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            sys::sleepMs(500);
        double now = sys::now();
        if (got > 0) {
            buffer[got] = 0;
            std::string text(buffer);
            size_t magic = strlen(BeaconMagic);
            size_t space = text.find(' ', magic + 1);
            if (text.compare(0, magic, BeaconMagic) == 0 && space != std::string::npos) {
                u16 port = (u16)atoi(text.c_str() + magic + 1);
                std::string name = text.substr(space + 1);
                // Its name goes in a path, so it keeps to what a folder name can hold.
                for (char& c : name) {
                    if (c == '/' || c == '\\' || c == ':' || (u8)c < ' ')
                        c = '-';
                }
                if (!name.empty() && port != 0) {
                    sys::Lock lock(g_heardMutex);
                    g_changed = g_changed || !g_heard.count(name);
                    g_heard[name] = Heard{from.sin_addr.s_addr, port, name, now};
                }
            }
        }
        sys::Lock lock(g_heardMutex);
        for (auto it = g_heard.begin(); it != g_heard.end();) {
            if (now - it->second.at > Forgotten) {
                it = g_heard.erase(it);
                g_changed = true;
            } else {
                ++it;
            }
        }
    }
    close(listening);
}

void startListening() {
    static std::atomic<bool> started{false};
    if (started.exchange(true) || !startSockets())
        return;
    // It listens for as long as the app is open.
    g_listener = new sys::Thread;
    g_listener->start([] { listenForPcs(); }, -1, 1 << 16, true);
}

/// The console's USB port, set up the once as a device of its own with one way in and one way out, as libnx's usbComms would have it, so the
/// driver the PC has for it is the same. Unlike usbComms, each transfer waits only so long and is then called off, so a PC that's gone leaves
/// nothing waiting.
/// When the cable last carried a request or an answer. A probe for the PC's sending service
/// shares the one USB endpoint and its buffer with installations and list scans, so it must not
/// cut into them.
std::atomic<double> g_lastUsbTraffic{0};

class UsbPort {
public:
    static UsbPort& get() {
        static UsbPort port;
        if (!port._ready)
            throw ToolError(port._problem);
        return port;
    }

    ~UsbPort() {
        if (_started)
            usbDsExit();
        free(_buffer);
    }

    /// Whether the PC has the console as its device, waiting as long as it's given for that.
    bool connected(u64 nanoseconds) {
        u64 deadline = armGetSystemTick() + armNsToTicks(nanoseconds);
        while (true) {
            UsbState state = UsbState_Detached;
            if (R_FAILED(usbDsGetState(&state)))
                return false;
            if (state == UsbState_Configured)
                return true;
            u64 now = armGetSystemTick();
            if (now >= deadline)
                return false;
            waitSingle(waiterForEvent(usbDsGetStateChangeEvent()), armTicksToNs(deadline - now));
            eventClear(usbDsGetStateChangeEvent());
        }
    }

    void write(const u8* data, size_t size, u64 wait = WaitNanoseconds) {
        g_lastUsbTraffic.store(sys::now(), std::memory_order_relaxed);
        while (size > 0) {
            size_t n = std::min(size, BufferSize);
            memcpy(_buffer, data, n);
            size_t sent = transfer(_in, n, wait);
            if (sent == 0)
                throw ToolError("USB 数据线已断开");
            data += sent;
            size -= sent;
        }
    }

    /// Exactly as much as is asked for, as the PC sends each part of an answer on its own and asking for what it comes to is what ends each one.
    void read(u8* data, size_t size, u64 wait = WaitNanoseconds) {
        g_lastUsbTraffic.store(sys::now(), std::memory_order_relaxed);
        constexpr int MostEmpty = 8;
        int empty = 0;
        while (size > 0) {
            size_t got = transfer(_out, std::min(size, BufferSize), wait);
            if (got == 0) {
                if (++empty > MostEmpty)
                    throw ToolError("USB 数据线已断开");
                continue;
            }
            empty = 0;
            memcpy(data, _buffer, got);
            data += got;
            size -= got;
        }
    }

    /// What the PC was asked for before a transfer was called off may still be on its way. It's taken in and left, until the PC goes
    /// quiet, so that the next answer is to the next question.
    void catchUp() {
        if (!_outOfStep)
            return;
        constexpr u64 Quiet = 300000000ULL;
        try {
            while (true)
                transfer(_out, BufferSize, Quiet);
        } catch (const ToolError&) {
        }
        _outOfStep = false;
    }

    static constexpr u64 WaitNanoseconds = 20000000000ULL;

private:
    static constexpr size_t BufferSize = 1 << 20;

    bool _outOfStep = false;

    UsbDsInterface* _interface = nullptr;
    UsbDsEndpoint* _in = nullptr;
    UsbDsEndpoint* _out = nullptr;
    /// usb:ds only takes buffers that start on a page.
    u8* _buffer = nullptr;
    bool _started = false, _ready = false;
    std::string _problem;

    UsbPort() {
        if (hosversionBefore(5, 0, 0)) {
            _problem = "通过 USB 数据线安装需要 5.0.0 或更高版本固件，请改用网络方式";
            return;
        }
        _buffer = (u8*)aligned_alloc(0x1000, BufferSize);
        if (!_buffer) {
            _problem = "内存不足，无法使用 USB 数据线";
            return;
        }
        if (R_FAILED(usbDsInitialize())) {
            _problem = "主机的 USB 端口当前被其他程序占用";
            return;
        }
        _started = true;
        Result rc = setUp();
        if (R_FAILED(rc)) {
            char code[64];
            snprintf(code, sizeof(code), " (%04u-%04u)", 2000 + R_MODULE(rc), R_DESCRIPTION(rc));
            _problem = std::string("无法设置主机的 USB 端口") + code;
            return;
        }
        _ready = true;
    }

    Result setUp() {
        Result rc;
        static const u16 English[] = {0x0409};
        u8 manufacturer = 0, product = 0, serial = 0;
        if (R_FAILED(rc = usbDsAddUsbLanguageStringDescriptor(nullptr, English, 1)) ||
            R_FAILED(rc = usbDsAddUsbStringDescriptor(&manufacturer, "Nintendo")) ||
            R_FAILED(rc = usbDsAddUsbStringDescriptor(&product, "Nintendo Switch")) ||
            R_FAILED(rc = usbDsAddUsbStringDescriptor(&serial, "SerialNumber")))
            return rc;

        usb_device_descriptor device = {};
        device.bLength = USB_DT_DEVICE_SIZE;
        device.bDescriptorType = USB_DT_DEVICE;
        device.bMaxPacketSize0 = 0x40;
        device.idVendor = 0x057E;
        device.idProduct = 0x3000;
        device.bcdDevice = 0x0100;
        device.iManufacturer = manufacturer;
        device.iProduct = product;
        device.iSerialNumber = serial;
        device.bNumConfigurations = 1;
        device.bcdUSB = 0x0110;
        if (R_FAILED(rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, &device)))
            return rc;
        device.bcdUSB = 0x0200;
        if (R_FAILED(rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &device)))
            return rc;
        // USB 3 gives the control packet size as a power of two: 2^9 is 512.
        device.bcdUSB = 0x0300;
        device.bMaxPacketSize0 = 0x09;
        if (R_FAILED(rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &device)))
            return rc;

        // What the console can do at each speed: USB 2's low-power links, and USB 3 from full speed up.
        static const u8 Capabilities[] = {
            0x05, USB_DT_BOS, 0x16, 0x00, 0x02,
            0x07, USB_DT_DEVICE_CAPABILITY, 0x02, 0x02, 0x00, 0x00, 0x00,
            0x0A, USB_DT_DEVICE_CAPABILITY, 0x03, 0x00, 0x0E, 0x00, 0x03, 0x00, 0x00, 0x00,
        };
        if (R_FAILED(rc = usbDsSetBinaryObjectStore(Capabilities, sizeof(Capabilities))))
            return rc;

        if (R_FAILED(rc = usbDsRegisterInterface(&_interface)))
            return rc;
        usb_interface_descriptor interface = {};
        interface.bLength = USB_DT_INTERFACE_SIZE;
        interface.bDescriptorType = USB_DT_INTERFACE;
        interface.bInterfaceNumber = _interface->interface_index;
        interface.bNumEndpoints = 2;
        interface.bInterfaceClass = USB_CLASS_VENDOR_SPEC;
        interface.bInterfaceSubClass = USB_CLASS_VENDOR_SPEC;
        interface.bInterfaceProtocol = USB_CLASS_VENDOR_SPEC;

        usb_endpoint_descriptor in = {};
        in.bLength = USB_DT_ENDPOINT_SIZE;
        in.bDescriptorType = USB_DT_ENDPOINT;
        in.bEndpointAddress = USB_ENDPOINT_IN | (u8)(interface.bInterfaceNumber + 1);
        in.bmAttributes = USB_TRANSFER_TYPE_BULK;
        usb_endpoint_descriptor out = in;
        out.bEndpointAddress = USB_ENDPOINT_OUT | (u8)(interface.bInterfaceNumber + 1);

        usb_ss_endpoint_companion_descriptor companion = {};
        companion.bLength = USB_DT_SS_ENDPOINT_COMPANION_SIZE;
        companion.bDescriptorType = USB_DT_SS_ENDPOINT_COMPANION;
        companion.bMaxBurst = 0x0F;

        // The largest packet at each speed.
        const struct {
            UsbDeviceSpeed speed;
            u16 packet;
        } Speeds[] = {{UsbDeviceSpeed_Full, 0x40}, {UsbDeviceSpeed_High, 0x200}, {UsbDeviceSpeed_Super, 0x400}};
        for (auto& each : Speeds) {
            in.wMaxPacketSize = out.wMaxPacketSize = each.packet;
            bool super = each.speed == UsbDeviceSpeed_Super;
            if (R_FAILED(rc = usbDsInterface_AppendConfigurationData(_interface, each.speed, &interface, USB_DT_INTERFACE_SIZE)) ||
                R_FAILED(rc = usbDsInterface_AppendConfigurationData(_interface, each.speed, &in, USB_DT_ENDPOINT_SIZE)) ||
                (super && R_FAILED(rc = usbDsInterface_AppendConfigurationData(_interface, each.speed, &companion, USB_DT_SS_ENDPOINT_COMPANION_SIZE))) ||
                R_FAILED(rc = usbDsInterface_AppendConfigurationData(_interface, each.speed, &out, USB_DT_ENDPOINT_SIZE)) ||
                (super && R_FAILED(rc = usbDsInterface_AppendConfigurationData(_interface, each.speed, &companion, USB_DT_SS_ENDPOINT_COMPANION_SIZE))))
                return rc;
        }

        if (R_FAILED(rc = usbDsInterface_RegisterEndpoint(_interface, &_in, in.bEndpointAddress)) ||
            R_FAILED(rc = usbDsInterface_RegisterEndpoint(_interface, &_out, out.bEndpointAddress)) ||
            R_FAILED(rc = usbDsInterface_EnableInterface(_interface)))
            return rc;
        return usbDsEnable();
    }

    /// One transfer through the buffer, of at most what's asked for, called off if it takes too long or the cable comes out.
    size_t transfer(UsbDsEndpoint* endpoint, size_t size, u64 wait) {
        u32 urb = 0;
        if (R_FAILED(usbDsEndpoint_PostBufferAsync(endpoint, _buffer, size, &urb)))
            throw ToolError("USB 数据线已断开");
        u64 deadline = armGetSystemTick() + armNsToTicks(wait);
        // A little at a time, so that whatever's waiting can give way when it's asked to.
        constexpr u64 Step = 100000000ULL;
        while (true) {
            u64 now = armGetSystemTick();
            bool givingWay = shouldGiveWay();
            s32 which = -1;
            Result rc = givingWay || now >= deadline ? KERNELRESULT(TimedOut)
                                                     : waitMulti(&which, std::min(armTicksToNs(deadline - now), Step), waiterForEvent(&endpoint->CompletionEvent),
                                                                 waiterForEvent(usbDsGetStateChangeEvent()));
            if (R_SUCCEEDED(rc) && which == 0)
                break;
            if (rc == KERNELRESULT(TimedOut) && !givingWay && armGetSystemTick() < deadline)
                continue;
            UsbState state = UsbState_Detached;
            if (R_SUCCEEDED(rc)) {
                // Something else about the cable changed, which only matters if it's no longer connected.
                eventClear(usbDsGetStateChangeEvent());
                if (R_SUCCEEDED(usbDsGetState(&state)) && state == UsbState_Configured)
                    continue;
            }
            usbDsEndpoint_Cancel(endpoint);
            // The transfer has stopped by the time it says it's finished, and only then can the
            // buffer be used again. Keep waiting for that event: reusing a buffer the console's
            // usb system module still has in flight corrupts its own state.
            for (int i = 0; i < 20; i++)
                if (R_SUCCEEDED(waitSingle(waiterForEvent(&endpoint->CompletionEvent), 100000000ULL)))
                    break;
            eventClear(&endpoint->CompletionEvent);
            // A cancelled URB can leave a report behind. Drain it while the cable is ours, so
            // the next transfer isn't matched against a stale entry.
            UsbDsReportData cancelled;
            usbDsEndpoint_GetReportData(endpoint, &cancelled);
            _outOfStep = true;
            if (givingWay)
                throw Cancelled();
            throw ToolError(R_SUCCEEDED(rc) ? "USB 数据线已断开" : "PC 已停止通过 USB 数据线应答");
        }
        eventClear(&endpoint->CompletionEvent);
        UsbDsReportData report;
        u32 requested = 0, transferred = 0;
        if (R_FAILED(usbDsEndpoint_GetReportData(endpoint, &report)) || R_FAILED(usbDsParseReportData(&report, urb, &requested, &transferred)))
            throw ToolError("USB 数据线已断开");
        // Refreshed on every completed transfer, not just at the start of a large read: a long
        // answer must never look like an idle cable to the service probe.
        g_lastUsbTraffic.store(sys::now(), std::memory_order_relaxed);
        return std::min<size_t>(transferred, size);
    }
};

/// A connection over the cable, to whatever `hacbrewpacklz send` is running on the PC at the other end.
class UsbTransport : public Transport {
public:
    UsbTransport() : _port(UsbPort::get()) {
        constexpr u64 ReadyNanoseconds = 3000000000ULL;
        if (!_port.connected(ReadyNanoseconds))
            throw ToolError("用 USB 数据线把主机连接到 PC，并在 PC 上运行 hacbrewpacklz send");
        _port.catchUp();
    }

    void send(const void* data, size_t size) override { _port.write((const u8*)data, size); }
    void receive(void* data, size_t size) override { _port.read((u8*)data, size); }

private:
    UsbPort& _port;
};

/// What's at the other end of the cable is only looked at once there's a list of PCs to show, as that sets the console's USB port up as this
/// app's own.
std::atomic<bool> g_usbLooked{false};
bool g_usbWasThere = false;
sys::Mutex g_usbMutex;
/// The name of the PC on the cable, asked for when it's plugged in and checked every few seconds
/// after that, so a service that stops sending is noticed without unplugging anything. Empty if
/// nothing answers, as when hacbrewpacklz send isn't running: then there's no PC on the cable to
/// look in, only a PC with a driver for the console.
std::string g_usbName;

bool usbToPc() {
    try {
        return UsbPort::get().connected(0);
    } catch (const ToolError&) {
        return false;
    }
}

/// Asks the PC on the cable what it's called, not waiting long, as nothing may be running there to answer.
/// False when an install owns the cable: the probe is skipped altogether, and the last name is
/// kept. The whole probe holds cableBusy, so it can never be posted alongside a request/answer.
bool askUsbName(std::string& name) {
    CableGuard probe(true, false);
    if (!probe.held())
        return false;
    constexpr u64 ShortWait = 1500000000ULL;
    try {
        UsbPort& port = UsbPort::get();
        port.catchUp();
        u8 request[28] = {'H', 'B', 'P', 'Q', 1};
        port.write(request, sizeof(request), ShortWait);
        u8 header[16];
        port.read(header, sizeof(header), ShortWait);
        u64 length = 0;
        memcpy(&length, header + 8, 8);
        name.clear();
        if (memcmp(header, "HBPR", 4) != 0 || length == 0 || length > 1024)
            return true;
        std::string text(length, '\0');
        port.read((u8*)text.data(), length, ShortWait);
        size_t line = text.find('\n');
        name = line == std::string::npos ? "" : text.substr(line + 1);
        for (char& c : name) {
            if (c == '/' || c == '\\' || c == ':' || (u8)c < ' ')
                c = '-';
        }
        return true;
    } catch (const ToolError&) {
        name.clear();
        return true;
    }
}

/// Asking the PC on the cable its name can take a few seconds when nothing there answers, so it's asked on a thread of its own, and
/// whoever wants the list of PCs never waits for it. It's asked again every few seconds once it has answered, so a service that
/// stops is noticed too. Either way the list has changed, and whoever looks after it can take another look.
void watchCable() {
    /// How often to retry while no service has answered, and how often to check one that has. The
    /// PC can stop sending with the cable still plugged in, so a name asked for only once would
    /// leave its games on the list until the cable was pulled and plugged back in.
    constexpr double AskAgainSeconds = 10;
    constexpr double AliveCheckSeconds = 3;
    /// A probe that went unanswered is retried shortly, not half a minute later: the PC
    /// service is usually started after the cable is plugged in, and its files must show up
    /// at once. The CableGuard in askUsbName() already skips the whole probe when an install
    /// owns the cable, so a long backoff only delayed discovery without protecting anything.
    constexpr double FailedAgainSeconds = 2;
    /// A probe goes through the same endpoint as everything else, so it waits until the cable has
    /// been quiet for this long, keeping out of the way of an install or a list scan. The guard
    /// in askUsbName() is what makes this safe; this is only about not holding up an install.
    constexpr double QuietSeconds = 2;
    bool asked = false;
    double askedAt = 0;
    bool lastFailed = false;
    while (!g_stopping) {
        bool there = usbToPc();
        bool ask = false;
        {
            sys::Lock lock(g_usbMutex);
            if (!there) {
                asked = false;
                lastFailed = false;
                if (!g_usbName.empty())
                    g_changed = true;
                g_usbName.clear();
            } else if (!asked) {
                asked = true;
                ask = true;
            } else {
                double now = sys::now();
                double interval = lastFailed ? FailedAgainSeconds
                                 : g_usbName.empty() ? AskAgainSeconds : AliveCheckSeconds;
                if (now - askedAt >= interval &&
                    now - g_lastUsbTraffic.load(std::memory_order_relaxed) >= QuietSeconds)
                    ask = true;
            }
        }
        if (ask) {
            std::string name;
            // A skipped probe leaves askedAt alone, so it's retried as soon as the cable is free.
            if (askUsbName(name)) {
                askedAt = sys::now();
                lastFailed = name.empty();
                sys::Lock lock(g_usbMutex);
                g_changed = g_changed || name != g_usbName;
                g_usbName = name;
            }
        }
        for (int i = 0; i < 5 && !g_stopping; i++)
            sys::sleepMs(100);
    }
}

void startWatchingCable() {
    static std::atomic<bool> started{false};
    if (started.exchange(true))
        return;
    // It watches for as long as the app is open.
    g_cableWatcher = new sys::Thread;
    g_cableWatcher->start([] { watchCable(); }, -1, 1 << 16, true);
}

}  // namespace

std::vector<Peer> peers(bool cable) {
    startListening();
    std::vector<Peer> list;
    {
        sys::Lock lock(g_heardMutex);
        for (auto& item : g_heard) {
            Heard heard = item.second;
            list.push_back(Peer{heard.name, "PC：" + heard.name + "（网络）", [heard] { return std::unique_ptr<Transport>(new TcpTransport(heard.address, heard.port)); },
                heard.name});
        }
    }
    // Only once a PC has the console as its device, which takes a cable plugged into it.
    if (!cable)
        return list;
    g_usbLooked = true;
    startWatchingCable();
    bool there = usbToPc();
    std::string name;
    {
        sys::Lock lock(g_usbMutex);
        g_usbWasThere = there;
        name = there ? g_usbName : std::string();
    }
    if (there && !name.empty())
        list.push_back(Peer{"USB", "PC：" + name + "（USB 数据线）", [] { return std::unique_ptr<Transport>(new UsbTransport()); }, name});
    preferCable(list);
    return list;
}

void stopLooking() {
    g_stopping = true;
    for (sys::Thread* thread : {g_listener, g_cableWatcher}) {
        if (thread)
            thread->join();
    }
}

bool peersChanged() {
    bool changed = g_changed.exchange(false);
    // A cable plugged in or pulled out changes the list too.
    if (g_usbLooked) {
        bool there = usbToPc();
        sys::Lock lock(g_usbMutex);
        changed = changed || there != g_usbWasThere;
    }
    return changed;
}

}  // namespace remote
}  // namespace hbp
