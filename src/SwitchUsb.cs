using System.Runtime.InteropServices;
using System.Text;

namespace HacBrewPackLz;

/// <summary>A Switch running hacBrewPackLZ, plugged in with a USB cable, through whichever of the WinUSB or libusbK drivers Windows has for it. Zadig
/// installs either, and DBI uses the same one, as the console shows up with the same IDs. Both drivers take the same calls.</summary>
internal sealed unsafe class SwitchUsb : IDisposable
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

    private SwitchUsb(nint file, nint usb, byte inPipe, byte outPipe, nint library, string prefix)
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
    public static SwitchUsb? Open(out string? problem)
    {
        problem = null;
        if (!OperatingSystem.IsWindows())
            return null;
        try
        {
            return Find(out problem);
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException or InvalidOperationException)
        {
            problem = e is EntryPointNotFoundException
                ? "无法打开 Switch 的 USB 连接：驱动缺少所需的函数，请用 Zadig 重新安装 WinUSB 或 libusbK 驱动。"
                : "无法打开 Switch 的 USB 连接：" + e.Message;
            return null;
        }
    }

    private static SwitchUsb? Find(out string? problem)
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
                        : $"检测到 Switch 已插入，但当前使用的是 {service} 驱动，请用 Zadig 改装 WinUSB 或 libusbK 驱动。";
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
            problem = $"Switch 驱动所需的 {library} 不存在，请用 Zadig 重新安装驱动。";
            return null;
        }
        const uint GenericReadWrite = 0xC0000000, ShareReadWrite = 3, OpenExisting = 3, Overlapped = 0x40000000, Normal = 0x80;
        nint file;
        fixed (char* p = path)
            file = CreateFileW(p, GenericReadWrite, ShareReadWrite, 0, OpenExisting, Normal | Overlapped, 0);
        if (file == -1)
            return null;
        // A driver with a missing export used to surface as a bare "Entry point was not found."
        foreach (string suffix in new[] { "Initialize", "QueryInterfaceSettings", "QueryPipe", "Free", "ReadPipe", "WritePipe" })
            if (!NativeLibrary.TryGetExport(lib, prefix + suffix, out _))
            {
                CloseHandle(file);
                problem = $"Switch 的 USB 驱动缺少 {prefix + suffix} 函数，请用 Zadig 重新安装 WinUSB 或 libusbK 驱动。";
                return null;
            }
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
        return new SwitchUsb(file, usb, inPipe, outPipe, lib, prefix);
    }

    /// <summary>One transfer's worth, which can be less than the buffer holds.</summary>
    public int Read(byte[] buffer, int offset, int count)
    {
        uint got = 0;
        fixed (byte* b = buffer)
        {
            if (_read(_usb, _in, b + offset, (uint)count, &got, 0) == 0)
                throw new IOException("Switch 的 USB 数据线已断开");
        }
        return (int)got;
    }

    public void Write(byte[] buffer, int offset, int count)
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

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return;
        _free(_usb);
        CloseHandle(_file);
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
