using System.Runtime.InteropServices;
using System.Text;

namespace HacBrewPackLz;

/// <summary>A Switch running hacBrewPackLZ, plugged in with a USB cable: on Windows through its own USB drivers, and on Linux and macOS through libusb.
/// Each read is one transfer's worth, which can be less than the buffer holds.</summary>
internal abstract class SwitchUsb : IDisposable
{
    /// <summary>The console's IDs, the same as DBI's, so the same driver works for both.</summary>
    public const ushort Vendor = 0x057E, Product = 0x3000;

    /// <summary>Null if there's no console there, with why not in problem when that's something to do anything about. HBP_USB=libusb takes the libusb
    /// way on Windows too, which is how it's tried with a console here.</summary>
    public static SwitchUsb? Open(out string? problem)
    {
        problem = null;
        try
        {
            if (OperatingSystem.IsWindows() && Environment.GetEnvironmentVariable("HBP_USB") != "libusb")
                return WindowsUsb.Find(out problem);
            return Possible ? LibUsb.Find(out problem) : null;
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException or InvalidOperationException)
        {
            problem = "无法打开 Switch 的 USB 连接：" + e.Message;
            return null;
        }
    }

    /// <summary>Whether this system can send over a cable at all.</summary>
    public static bool Possible => OperatingSystem.IsWindows() || OperatingSystem.IsLinux() || OperatingSystem.IsMacOS();

    public abstract int Read(byte[] buffer, int offset, int count);
    public abstract void Write(byte[] buffer, int offset, int count);
    public abstract void Dispose();
}

/// <summary>Windows, through whichever of the WinUSB or libusbK drivers it has for the console. Zadig installs either, and DBI uses the same one, as
/// the console shows up with the same IDs. Both drivers take the same calls.</summary>
internal sealed unsafe class WindowsUsb : SwitchUsb
{
    private const string HardwareId = "VID_057E&PID_3000";
    private const uint DigcfPresent = 0x2, DigcfAllClasses = 0x4, DigcfDeviceInterface = 0x10;
    private const uint SpdrpHardwareId = 1, SpdrpService = 4;

    [StructLayout(LayoutKind.Sequential)]
    private struct DeviceInfo
    {
        public uint Size;
        public Guid Class;
        public uint Instance;
        public nint Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct InterfaceData
    {
        public uint Size;
        public Guid Class;
        public uint Flags;
        public nint Reserved;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    private struct InterfaceDescriptor
    {
        public byte Length, DescriptorType, InterfaceNumber, AlternateSetting, NumEndpoints, InterfaceClass, InterfaceSubClass, InterfaceProtocol, Interface;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PipeInformation
    {
        public int PipeType;
        public byte PipeId;
        public ushort MaximumPacketSize;
        public byte Interval;
    }

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern nint SetupDiGetClassDevsW(Guid* classGuid, char* enumerator, nint parent, uint flags);
    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern int SetupDiEnumDeviceInfo(nint set, uint index, DeviceInfo* info);
    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern int SetupDiGetDeviceRegistryPropertyW(nint set, DeviceInfo* info, uint property, uint* type, byte* buffer, uint size, uint* needed);
    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern nint SetupDiOpenDevRegKey(nint set, DeviceInfo* info, uint scope, uint profile, uint keyType, uint access);
    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern int SetupDiEnumDeviceInterfaces(nint set, DeviceInfo* info, Guid* guid, uint index, InterfaceData* data);
    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern int SetupDiGetDeviceInterfaceDetailW(nint set, InterfaceData* data, byte* detail, uint size, uint* needed, DeviceInfo* info);
    [DllImport("setupapi.dll")]
    private static extern int SetupDiDestroyDeviceInfoList(nint set);
    [DllImport("advapi32.dll")]
    private static extern int RegQueryValueExW(nint key, char* name, nint reserved, uint* type, byte* data, uint* size);
    [DllImport("advapi32.dll")]
    private static extern int RegCloseKey(nint key);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern nint CreateFileW(char* name, uint access, uint share, nint security, uint creation, uint flags, nint template);
    [DllImport("kernel32.dll")]
    private static extern int CloseHandle(nint handle);

    private nint _file;
    private nint _usb;
    private byte _in, _out;
    private readonly delegate* unmanaged[Stdcall]<nint, byte, byte*, uint, uint*, nint, int> _read;
    private readonly delegate* unmanaged[Stdcall]<nint, byte, byte*, uint, uint*, nint, int> _write;
    private readonly delegate* unmanaged[Stdcall]<nint, int> _free;
    private int _disposed;

    private WindowsUsb(nint file, nint usb, byte inPipe, byte outPipe, nint library, string prefix)
    {
        _file = file;
        _usb = usb;
        _in = inPipe;
        _out = outPipe;
        _read = (delegate* unmanaged[Stdcall]<nint, byte, byte*, uint, uint*, nint, int>)NativeLibrary.GetExport(library, prefix + "ReadPipe");
        _write = (delegate* unmanaged[Stdcall]<nint, byte, byte*, uint, uint*, nint, int>)NativeLibrary.GetExport(library, prefix + "WritePipe");
        _free = (delegate* unmanaged[Stdcall]<nint, int>)NativeLibrary.GetExport(library, prefix + "Free");
    }

    /// <summary>Null if there's no console there, with why not in problem when that's something to do anything about.</summary>
    internal static SwitchUsb? Find(out string? problem)
    {
        problem = null;
        nint set;
        fixed (char* usb = "USB")
            set = SetupDiGetClassDevsW(null, usb, 0, DigcfPresent | DigcfAllClasses);
        if (set == -1)
            return null;
        try
        {
            var info = new DeviceInfo { Size = (uint)sizeof(DeviceInfo) };
            for (uint index = 0; SetupDiEnumDeviceInfo(set, index, &info) != 0; index++)
            {
                if (!Property(set, &info, SpdrpHardwareId).Contains(HardwareId, StringComparison.OrdinalIgnoreCase))
                    continue;
                string service = Property(set, &info, SpdrpService);
                string prefix = service.Equals("WinUSB", StringComparison.OrdinalIgnoreCase) ? "WinUsb_" : service.Equals("libusbK", StringComparison.OrdinalIgnoreCase) ? "UsbK_" : "";
                if (prefix.Length == 0)
                {
                    problem = service.Length == 0
                        ? "检测到 Switch 已插入，但 Windows 未安装其驱动，请用 Zadig 安装 WinUSB 驱动。"
                        : $"A Switch is plugged in, but with the {service} driver. Install the WinUSB or libusbK driver for it with Zadig instead.";
                    return null;
                }
                Guid? guid = InterfaceGuid(set, &info);
                string? path = guid is { } g ? DevicePath(g) : null;
                if (path is null)
                {
                    problem = "检测到 Switch 已插入，但无法访问其驱动，请用 Zadig 重新安装驱动。";
                    return null;
                }
                return Connect(path, prefix == "WinUsb_" ? "winusb.dll" : "libusbK.dll", prefix, out problem);
            }
            return null;
        }
        finally
        {
            SetupDiDestroyDeviceInfoList(set);
        }
    }

    private static string Property(nint set, DeviceInfo* info, uint property)
    {
        var buffer = new byte[2048];
        uint type, needed;
        fixed (byte* b = buffer)
        {
            if (SetupDiGetDeviceRegistryPropertyW(set, info, property, &type, b, (uint)buffer.Length, &needed) == 0)
                return "";
        }
        return Encoding.Unicode.GetString(buffer, 0, (int)Math.Min(needed, (uint)buffer.Length)).Replace('\0', '\n').Trim('\n');
    }

    private static Guid? InterfaceGuid(nint set, DeviceInfo* info)
    {
        const uint KeyRead = 0x20019;
        nint key = SetupDiOpenDevRegKey(set, info, 1, 0, 1, KeyRead);
        if (key == -1 || key == 0)
            return null;
        try
        {
            foreach (var name in new[] { "DeviceInterfaceGUIDs", "DeviceInterfaceGUID" })
            {
                var buffer = new byte[1024];
                uint type, size = (uint)buffer.Length;
                int result;
                fixed (char* n = name)
                fixed (byte* b = buffer)
                    result = RegQueryValueExW(key, n, 0, &type, b, &size);
                if (result != 0)
                    continue;
                string text = Encoding.Unicode.GetString(buffer, 0, (int)size).Split('\0', StringSplitOptions.RemoveEmptyEntries).FirstOrDefault() ?? "";
                if (Guid.TryParse(text, out var guid))
                    return guid;
            }
            return null;
        }
        finally
        {
            RegCloseKey(key);
        }
    }

    private static string? DevicePath(Guid guid)
    {
        nint set = SetupDiGetClassDevsW(&guid, null, 0, DigcfPresent | DigcfDeviceInterface);
        if (set == -1)
            return null;
        try
        {
            var data = new InterfaceData { Size = (uint)sizeof(InterfaceData) };
            if (SetupDiEnumDeviceInterfaces(set, null, &guid, 0, &data) == 0)
                return null;
            uint needed = 0;
            SetupDiGetDeviceInterfaceDetailW(set, &data, null, 0, &needed, null);
            if (needed == 0)
                return null;
            var detail = new byte[needed];
            fixed (byte* d = detail)
            {
                // The size of the fixed part, which is padded on 64-bit Windows.
                *(uint*)d = IntPtr.Size == 8 ? 8u : 6u;
                if (SetupDiGetDeviceInterfaceDetailW(set, &data, d, needed, &needed, null) == 0)
                    return null;
                return new string((char*)(d + 4));
            }
        }
        finally
        {
            SetupDiDestroyDeviceInfoList(set);
        }
    }

    private static SwitchUsb? Connect(string path, string library, string prefix, out string? problem)
    {
        problem = null;
        if (!NativeLibrary.TryLoad(library, out nint lib))
        {
            problem = $"The Switch's driver needs {library}, which isn't there.";
            return null;
        }
        const uint GenericReadWrite = 0xC0000000, ShareReadWrite = 3, OpenExisting = 3, Overlapped = 0x40000000, Normal = 0x80;
        nint file;
        fixed (char* p = path)
            file = CreateFileW(p, GenericReadWrite, ShareReadWrite, 0, OpenExisting, Normal | Overlapped, 0);
        if (file == -1)
            return null;
        var initialize = (delegate* unmanaged[Stdcall]<nint, nint*, int>)NativeLibrary.GetExport(lib, prefix + "Initialize");
        var querySettings = (delegate* unmanaged[Stdcall]<nint, byte, InterfaceDescriptor*, int>)NativeLibrary.GetExport(lib, prefix + "QueryInterfaceSettings");
        var queryPipe = (delegate* unmanaged[Stdcall]<nint, byte, byte, PipeInformation*, int>)NativeLibrary.GetExport(lib, prefix + "QueryPipe");
        var free = (delegate* unmanaged[Stdcall]<nint, int>)NativeLibrary.GetExport(lib, prefix + "Free");
        nint usb;
        if (initialize(file, &usb) == 0)
        {
            CloseHandle(file);
            problem = "Switch 已插入，但无法打开其 USB 连接。";
            return null;
        }
        InterfaceDescriptor descriptor;
        byte inPipe = 0, outPipe = 0;
        if (querySettings(usb, 0, &descriptor) != 0)
        {
            for (byte i = 0; i < descriptor.NumEndpoints; i++)
            {
                PipeInformation pipe;
                if (queryPipe(usb, 0, i, &pipe) == 0 || pipe.PipeType != 2)
                    continue;
                if ((pipe.PipeId & 0x80) != 0)
                    inPipe = pipe.PipeId;
                else
                    outPipe = pipe.PipeId;
            }
        }
        if (inPipe == 0 || outPipe == 0)
        {
            free(usb);
            CloseHandle(file);
            problem = "Switch 已插入，但上面没有打开 hacBrewPackLZ。";
            return null;
        }
        // Nothing to end what's sent with an empty packet: each side reads exactly as much as the other sends, which is what ends each read.
        return new WindowsUsb(file, usb, inPipe, outPipe, lib, prefix);
    }

    /// <summary>One transfer's worth, which can be less than the buffer holds.</summary>
    public override int Read(byte[] buffer, int offset, int count)
    {
        uint got = 0;
        fixed (byte* b = buffer)
        {
            if (_read(_usb, _in, b + offset, (uint)count, &got, 0) == 0)
                throw new IOException("Switch 的 USB 数据线已断开");
        }
        return (int)got;
    }

    public override void Write(byte[] buffer, int offset, int count)
    {
        while (count > 0)
        {
            uint sent = 0;
            fixed (byte* b = buffer)
            {
                if (_write(_usb, _out, b + offset, (uint)count, &sent, 0) == 0 || sent == 0)
                    throw new IOException("Switch 的 USB 数据线已断开");
            }
            offset += (int)sent;
            count -= (int)sent;
        }
    }

    public override void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return;
        _free(_usb);
        CloseHandle(_file);
    }
}

/// <summary>Linux and macOS, through libusb, which needs no driver installed: on Linux, only that this user is allowed to use the console, which a
/// udev rule gives once (Help says how). A transfer can't be called off from another thread, so each one waits a little at a time, and closing waits
/// for the one in progress to see that it's closing.</summary>
internal sealed unsafe class LibUsb : SwitchUsb
{
    private const int ErrorAccess = -3, ErrorBusy = -6, ErrorTimeout = -7;
    private const uint Patience = 1000;

    [StructLayout(LayoutKind.Sequential)]
    private struct DeviceDescriptor
    {
        public byte Length, DescriptorType;
        public ushort Usb;
        public byte DeviceClass, DeviceSubClass, DeviceProtocol, MaxPacketSize0;
        public ushort Vendor, Product, Release;
        public byte Manufacturer, ProductName, SerialNumber, Configurations;
    }

    /// <summary>libusb's calls, loaded once for as long as the program runs.</summary>
    private sealed class Api
    {
        public readonly nint Context;
        public readonly delegate* unmanaged<nint, nint**, nint> GetDeviceList;
        public readonly delegate* unmanaged<nint*, int, void> FreeDeviceList;
        public readonly delegate* unmanaged<nint, DeviceDescriptor*, int> GetDeviceDescriptor;
        public readonly delegate* unmanaged<nint, nint*, int> OpenDevice;
        public readonly delegate* unmanaged<nint, void> Close;
        public readonly delegate* unmanaged<nint, int, int> SetAutoDetachKernelDriver;
        public readonly delegate* unmanaged<nint, int, int> ClaimInterface;
        public readonly delegate* unmanaged<nint, int, int> ReleaseInterface;
        public readonly delegate* unmanaged<nint, byte, byte, ushort, ushort, byte*, ushort, uint, int> ControlTransfer;
        public readonly delegate* unmanaged<nint, byte, byte*, int, int*, uint, int> BulkTransfer;
        private readonly delegate* unmanaged<int, byte*> _errorName;

        public Api(nint library, nint context)
        {
            Context = context;
            GetDeviceList = (delegate* unmanaged<nint, nint**, nint>)NativeLibrary.GetExport(library, "libusb_get_device_list");
            FreeDeviceList = (delegate* unmanaged<nint*, int, void>)NativeLibrary.GetExport(library, "libusb_free_device_list");
            GetDeviceDescriptor = (delegate* unmanaged<nint, DeviceDescriptor*, int>)NativeLibrary.GetExport(library, "libusb_get_device_descriptor");
            OpenDevice = (delegate* unmanaged<nint, nint*, int>)NativeLibrary.GetExport(library, "libusb_open");
            Close = (delegate* unmanaged<nint, void>)NativeLibrary.GetExport(library, "libusb_close");
            SetAutoDetachKernelDriver = (delegate* unmanaged<nint, int, int>)NativeLibrary.GetExport(library, "libusb_set_auto_detach_kernel_driver");
            ClaimInterface = (delegate* unmanaged<nint, int, int>)NativeLibrary.GetExport(library, "libusb_claim_interface");
            ReleaseInterface = (delegate* unmanaged<nint, int, int>)NativeLibrary.GetExport(library, "libusb_release_interface");
            ControlTransfer = (delegate* unmanaged<nint, byte, byte, ushort, ushort, byte*, ushort, uint, int>)NativeLibrary.GetExport(library, "libusb_control_transfer");
            BulkTransfer = (delegate* unmanaged<nint, byte, byte*, int, int*, uint, int>)NativeLibrary.GetExport(library, "libusb_bulk_transfer");
            _errorName = (delegate* unmanaged<int, byte*>)NativeLibrary.GetExport(library, "libusb_error_name");
        }

        public string ErrorName(int code) => Marshal.PtrToStringAnsi((nint)_errorName(code)) ?? code.ToString();
    }

    private static readonly object Loading = new();
    private static Api? _api;
    private static string? _missing;

    private readonly Api _lib;
    private readonly nint _handle;
    private readonly int _interface;
    private readonly byte _in, _out;
    private readonly object _gate = new();
    private int _transfers;
    private volatile bool _closing;
    private bool _closed;

    private LibUsb(Api lib, nint handle, int interfaceNumber, byte inPoint, byte outPoint)
    {
        _lib = lib;
        _handle = handle;
        _interface = interfaceNumber;
        _in = inPoint;
        _out = outPoint;
    }

    /// <summary>libusb is part of nearly every Linux. On a Mac it comes beside the program, or from Homebrew.</summary>
    private static Api? Load(out string? problem)
    {
        lock (Loading)
        {
            if (_api is null && _missing is null)
            {
                string[] names = OperatingSystem.IsMacOS()
                    ? ["libusb-1.0.0.dylib", "/opt/homebrew/lib/libusb-1.0.0.dylib", "/usr/local/lib/libusb-1.0.0.dylib", "libusb-1.0.dylib"]
                    : OperatingSystem.IsWindows() ? ["libusb-1.0.dll"] : ["libusb-1.0.so.0", "libusb-1.0.so"];
                nint library = 0;
                foreach (var name in names)
                {
                    bool loaded = Path.IsPathRooted(name)
                        ? NativeLibrary.TryLoad(name, out library)
                        : NativeLibrary.TryLoad(name, typeof(LibUsb).Assembly, null, out library);
                    if (loaded)
                        break;
                }
                if (library == 0)
                    _missing = OperatingSystem.IsMacOS() ? "Sending by USB cable needs libusb, which Homebrew installs: brew install libusb"
                        : OperatingSystem.IsWindows() ? "Sending by USB cable this way needs libusb-1.0.dll beside the program"
                        : "Sending by USB cable needs libusb, which isn't installed: install the libusb-1.0 package";
                else
                {
                    var init = (delegate* unmanaged<nint*, int>)NativeLibrary.GetExport(library, "libusb_init");
                    nint context = 0;
                    int rc = init(&context);
                    if (rc != 0)
                        _missing = $"libusb couldn't start ({rc})";
                    else
                        _api = new Api(library, context);
                }
            }
            problem = _missing;
            return _api;
        }
    }

    internal static SwitchUsb? Find(out string? problem)
    {
        var lib = Load(out problem);
        if (lib is null)
            return null;
        nint* list;
        nint count = lib.GetDeviceList(lib.Context, &list);
        if (count < 0)
            return null;
        try
        {
            for (nint i = 0; i < count; i++)
            {
                DeviceDescriptor device;
                if (lib.GetDeviceDescriptor(list[i], &device) != 0 || device.Vendor != Vendor || device.Product != Product)
                    continue;
                nint handle;
                int rc = lib.OpenDevice(list[i], &handle);
                if (rc == ErrorAccess)
                {
                    problem = OperatingSystem.IsLinux()
                        ? "A Switch is plugged in, but this user isn't allowed to use it. Help says how to allow it, once"
                        : "A Switch is plugged in, but this user isn't allowed to use it";
                    return null;
                }
                if (rc != 0)
                {
                    problem = $"A Switch is plugged in, but its USB connection couldn't be opened ({lib.ErrorName(rc)})";
                    return null;
                }
                return Connect(lib, handle, out problem);
            }
            return null;
        }
        finally
        {
            lib.FreeDeviceList(list, 1);
        }
    }

    private static SwitchUsb? Connect(Api lib, nint handle, out string? problem)
    {
        problem = null;
        // The configuration's descriptors as the console sends them, read straight, so nothing depends on how libusb lays them out.
        var descriptors = new byte[256];
        int got;
        fixed (byte* d = descriptors)
            got = lib.ControlTransfer(handle, 0x80, 6, 0x0200, 0, d, (ushort)descriptors.Length, Patience);
        if (got <= 0 || !Endpoints(descriptors.AsSpan(0, got), out int interfaceNumber, out byte inPoint, out byte outPoint))
        {
            lib.Close(handle);
            problem = "The Switch is plugged in, but not with hacBrewPackLZ open on it";
            return null;
        }
        // Only Linux can take the console from a driver that has it; elsewhere this says it can't, which is fine.
        lib.SetAutoDetachKernelDriver(handle, 1);
        int rc = lib.ClaimInterface(handle, interfaceNumber);
        if (rc != 0)
        {
            lib.Close(handle);
            problem = rc == ErrorBusy ? "A Switch is plugged in, but another program is using its USB connection"
                : $"A Switch is plugged in, but its USB connection couldn't be opened ({lib.ErrorName(rc)})";
            return null;
        }
        return new LibUsb(lib, handle, interfaceNumber, inPoint, outPoint);
    }

    /// <summary>The first interface in a configuration's descriptors, and its bulk endpoint each way. A USB 3 companion after each endpoint is
    /// passed over, as is anything else.</summary>
    internal static bool Endpoints(ReadOnlySpan<byte> descriptors, out int interfaceNumber, out byte inPoint, out byte outPoint)
    {
        interfaceNumber = -1;
        inPoint = outPoint = 0;
        bool inFirst = false;
        for (int at = 0; at + 2 <= descriptors.Length;)
        {
            int length = descriptors[at], type = descriptors[at + 1];
            if (length < 2 || at + length > descriptors.Length)
                break;
            if (type == 4 && length >= 9)
            {
                if (interfaceNumber >= 0)
                    break;
                inFirst = descriptors[at + 3] == 0;
                if (inFirst)
                    interfaceNumber = descriptors[at + 2];
            }
            else if (type == 5 && length >= 7 && inFirst && (descriptors[at + 3] & 3) == 2)
            {
                byte address = descriptors[at + 2];
                if ((address & 0x80) != 0)
                {
                    if (inPoint == 0)
                        inPoint = address;
                }
                else if (outPoint == 0)
                    outPoint = address;
            }
            at += length;
        }
        return interfaceNumber >= 0 && inPoint != 0 && outPoint != 0;
    }

    public override int Read(byte[] buffer, int offset, int count)
    {
        Begin();
        try
        {
            fixed (byte* b = buffer)
            {
                while (true)
                {
                    int got = 0;
                    int rc = _lib.BulkTransfer(_handle, _in, b + offset, count, &got, Patience);
                    // What came before the wait ran out still counts: the reader asks again for the rest.
                    if (rc == 0 || got > 0)
                        return got;
                    if (rc != ErrorTimeout || _closing)
                        throw new IOException("Switch 的 USB 数据线已断开");
                }
            }
        }
        finally
        {
            End();
        }
    }

    public override void Write(byte[] buffer, int offset, int count)
    {
        Begin();
        try
        {
            fixed (byte* b = buffer)
            {
                while (count > 0)
                {
                    int sent = 0;
                    int rc = _lib.BulkTransfer(_handle, _out, b + offset, count, &sent, Patience);
                    if (rc != 0 && (rc != ErrorTimeout || _closing))
                        throw new IOException("Switch 的 USB 数据线已断开");
                    offset += sent;
                    count -= sent;
                }
            }
        }
        finally
        {
            End();
        }
    }

    private void Begin()
    {
        lock (_gate)
        {
            if (_closing)
                throw new IOException("Switch 的 USB 数据线已断开");
            _transfers++;
        }
    }

    private void End()
    {
        lock (_gate)
        {
            _transfers--;
            if (!_closing || _transfers > 0 || _closed)
                return;
            _closed = true;
        }
        Shut();
    }

    public override void Dispose()
    {
        lock (_gate)
        {
            _closing = true;
            if (_transfers > 0 || _closed)
                return;
            _closed = true;
        }
        Shut();
    }

    private void Shut()
    {
        _lib.ReleaseInterface(_handle, _interface);
        _lib.Close(_handle);
    }
}

/// <summary>The USB connection as a stream. The console sends each part of a request on its own, and each read asks for exactly what that part comes
/// to, which is what ends it.</summary>
internal sealed class UsbStream(SwitchUsb device) : Stream
{
    public override int Read(byte[] buffer, int offset, int count) => count == 0 ? 0 : device.Read(buffer, offset, count);

    public override void Write(byte[] buffer, int offset, int count) => device.Write(buffer, offset, count);
    public override void Flush() { }
    public override bool CanRead => true;
    public override bool CanWrite => true;
    public override bool CanSeek => false;
    public override long Length => throw new NotSupportedException();
    public override long Position { get => throw new NotSupportedException(); set => throw new NotSupportedException(); }
    public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
    public override void SetLength(long value) => throw new NotSupportedException();
}
