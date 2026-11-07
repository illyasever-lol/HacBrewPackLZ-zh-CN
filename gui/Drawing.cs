using System.Runtime.InteropServices;

namespace HacBrewPackLz;

internal readonly record struct Rgb(byte R, byte G, byte B)
{
    public static Rgb Hex(uint value) => new((byte)(value >> 16), (byte)(value >> 8), (byte)value);
    public uint Gdi => (uint)(R | (G << 8) | (B << 16));
    public uint Argb => 0xFF000000u | ((uint)R << 16) | ((uint)G << 8) | B;
}

internal readonly record struct Rect(int X, int Y, int W, int H)
{
    public int Right => X + W;
    public int Bottom => Y + H;
    public bool Contains(int x, int y) => x >= X && y >= Y && x < Right && y < Bottom;
    public Rect Inset(int dx, int dy) => new(X + dx, Y + dy, W - 2 * dx, H - 2 * dy);
    public Rect Inset(int d) => Inset(d, d);
}

internal readonly record struct Font(int Size, string Family = "Segoe UI");

internal static class Fonts
{
    public const string Semibold = "Segoe UI Semibold";
    public const string IconFamily = "Segoe MDL2 Assets";

    public static readonly Font Body = new(14);
    public static readonly Font Strong = new(14, Semibold);
    public static readonly Font Small = new(12);
    public static readonly Font Heading = new(16, Semibold);
    public static readonly Font Title = new(20, Semibold);
    public static readonly Font Hero = new(18, Semibold);

    public static Font Icon(int size) => new(size, IconFamily);
}

/// <summary>Glyphs from Segoe MDL2 Assets, which every Windows 10 and 11 has.</summary>
internal static class Glyph
{
    public const char Add = '';
    public const char Folder = '';
    public const char FolderOpen = '';
    public const char Search = '';
    public const char Settings = '';
    public const char More = '';
    public const char Close = '';
    public const char Check = '';
    public const char Warning = '';
    public const char Info = '';
    public const char Download = '';
    public const char ChevronDown = '';
    public const char Stop = '';
    public const char Document = '';
}

internal sealed record Palette(
    Rgb Background, Rgb Surface, Rgb Hover, Rgb Pressed, Rgb Border, Rgb StrongBorder,
    Rgb Text, Rgb SubText, Rgb Faint, Rgb Accent, Rgb AccentHover, Rgb AccentPressed, Rgb OnAccent,
    Rgb Good, Rgb Warn, Rgb Bad, Rgb WarnSurface);

/// <summary>Follows the light or dark choice in Windows settings.</summary>
internal static class Theme
{
    // Text is at least 4.5:1 against whatever it's drawn on, and outlines of things to click at least 3:1.
    private static readonly Palette Light = new(
        Rgb.Hex(0xE6E8EB), Rgb.Hex(0xFFFFFF), Rgb.Hex(0xDADDE2), Rgb.Hex(0xCBCFD5), Rgb.Hex(0xC9CDD3), Rgb.Hex(0x868D96),
        Rgb.Hex(0x15171A), Rgb.Hex(0x454A51), Rgb.Hex(0x61676F), Rgb.Hex(0x0A6878), Rgb.Hex(0x0B7285), Rgb.Hex(0x07505C), Rgb.Hex(0xFFFFFF),
        Rgb.Hex(0x1B6E34), Rgb.Hex(0x865A00), Rgb.Hex(0xB42222), Rgb.Hex(0xFCE9C0));

    private static readonly Palette Dark = new(
        Rgb.Hex(0x1C1D20), Rgb.Hex(0x26282C), Rgb.Hex(0x2E3035), Rgb.Hex(0x36393E), Rgb.Hex(0x35383D), Rgb.Hex(0x50545B),
        Rgb.Hex(0xECEDEF), Rgb.Hex(0xA4A8AF), Rgb.Hex(0x70747B), Rgb.Hex(0x3FB8C6), Rgb.Hex(0x58C5D2), Rgb.Hex(0x31A3B1), Rgb.Hex(0x0A2226),
        Rgb.Hex(0x6DCB8C), Rgb.Hex(0xE3B566), Rgb.Hex(0xEF7F79), Rgb.Hex(0x36301F));

    public static bool IsDark { get; private set; }
    public static Palette P { get; private set; } = Light;

    public static void Refresh()
    {
        int size = sizeof(int);
        int error = Win32.RegGetValueW(Win32.HKEY_CURRENT_USER, @"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize",
            "AppsUseLightTheme", Win32.RRF_RT_REG_DWORD, IntPtr.Zero, out int light, ref size);
        string? forced = Environment.GetEnvironmentVariable("HACBREWPACKLZ_GUI_THEME");
        IsDark = forced is not null ? forced == "dark" : error == 0 && light == 0;
        P = IsDark ? Dark : Light;
    }
}

[Flags]
internal enum TextFlags : uint
{
    Left = 0,
    Center = 1,
    Right = 2,
    VCenter = 4,
    Bottom = 8,
    WordBreak = 0x10,
    SingleLine = 0x20,
    NoPrefix = 0x800,
    EditControl = 0x2000,
    PathEllipsis = 0x4000,
    EndEllipsis = 0x8000,
    WordEllipsis = 0x40000,

    Line = SingleLine | VCenter | EndEllipsis | NoPrefix,
    Path = SingleLine | VCenter | PathEllipsis | NoPrefix,
    Wrapped = WordBreak | EditControl | WordEllipsis | NoPrefix,
}

/// <summary>Drawing onto a window's back buffer. Shapes use GDI+ for smooth corners, and text uses GDI for ClearType.</summary>
internal sealed class Gfx : IDisposable
{
    private const int SmoothingAntiAlias = 4;
    private const int PixelOffsetHalf = 4;
    private const int UnitPixel = 2;
    private const int FillModeWinding = 1;

    private static readonly Dictionary<uint, IntPtr> Brushes = [];
    private static readonly Dictionary<(Font font, int dpi), IntPtr> FontHandles = [];

    private readonly IntPtr _dc;
    private readonly IntPtr _graphics;

    public int Dpi { get; }

    public Gfx(IntPtr dc, int dpi)
    {
        _dc = dc;
        Dpi = dpi;
        Win32.GdipCreateFromHDC(dc, out _graphics);
        Win32.GdipSetSmoothingMode(_graphics, SmoothingAntiAlias);
        Win32.GdipSetPixelOffsetMode(_graphics, PixelOffsetHalf);
        Win32.SetBkMode(dc, Win32.TRANSPARENT);
    }

    public void Dispose() => Win32.GdipDeleteGraphics(_graphics);

    public int S(int value) => value * Dpi / 96;

    public static IntPtr FontHandle(Font font, int dpi)
    {
        if (!FontHandles.TryGetValue((font, dpi), out var handle))
        {
            const uint DefaultCharset = 1;
            const uint ClearTypeQuality = 5;
            handle = Win32.CreateFontW(-(font.Size * dpi / 96), 0, 0, 0, 400, 0, 0, 0, DefaultCharset, 0, 0, ClearTypeQuality, 0, font.Family);
            FontHandles[(font, dpi)] = handle;
        }
        return handle;
    }

    public static IntPtr Brush(Rgb color)
    {
        if (!Brushes.TryGetValue(color.Gdi, out var brush))
        {
            brush = Win32.CreateSolidBrush(color.Gdi);
            Brushes[color.Gdi] = brush;
        }
        return brush;
    }

    public void Fill(Rect r, Rgb color)
    {
        var rect = new RECT { Left = r.X, Top = r.Y, Right = r.Right, Bottom = r.Bottom };
        Win32.FillRect(_dc, ref rect, Brush(color));
    }

    public void FillRound(Rect r, float radius, Rgb color)
    {
        var path = RoundPath(r.X, r.Y, r.W, r.H, radius);
        Win32.GdipCreateSolidFill(color.Argb, out var brush);
        Win32.GdipFillPath(_graphics, brush, path);
        Win32.GdipDeleteBrush(brush);
        Win32.GdipDeletePath(path);
    }

    public void StrokeRound(Rect r, float radius, Rgb color, float width = 1)
    {
        float inset = width / 2;
        var path = RoundPath(r.X + inset, r.Y + inset, r.W - width, r.H - width, radius);
        Win32.GdipCreatePen1(color.Argb, width, UnitPixel, out var pen);
        Win32.GdipDrawPath(_graphics, pen, path);
        Win32.GdipDeletePen(pen);
        Win32.GdipDeletePath(path);
    }

    public void FillCircle(float x, float y, float diameter, Rgb color)
    {
        Win32.GdipCreateSolidFill(color.Argb, out var brush);
        Win32.GdipFillEllipse(_graphics, brush, x, y, diameter, diameter);
        Win32.GdipDeleteBrush(brush);
    }

    public void Text(string text, Rect r, Font font, Rgb color, TextFlags flags = TextFlags.Line)
    {
        if (text.Length == 0)
            return;
        Win32.GdipFlush(_graphics, 0);
        var old = Win32.SelectObject(_dc, FontHandle(font, Dpi));
        Win32.SetTextColor(_dc, color.Gdi);
        var rect = new RECT { Left = r.X, Top = r.Y, Right = r.Right, Bottom = r.Bottom };
        Win32.DrawTextW(_dc, text, text.Length, ref rect, (uint)flags);
        Win32.SelectObject(_dc, old);
    }

    public void Icon(char glyph, Rect r, int size, Rgb color) =>
        Text(glyph.ToString(), r, Fonts.Icon(size), color, TextFlags.SingleLine | TextFlags.Center | TextFlags.VCenter | TextFlags.NoPrefix);

    public void Image(Picture picture, Rect r)
    {
        const int HighQualityBicubic = 7;
        Win32.GdipSetInterpolationMode(_graphics, HighQualityBicubic);
        Win32.GdipDrawImageRectI(_graphics, picture.Handle, r.X, r.Y, r.W, r.H);
    }

    public void Clip(Rect r)
    {
        Win32.GdipFlush(_graphics, 0);
        Win32.IntersectClipRect(_dc, r.X, r.Y, r.Right, r.Bottom);
        Win32.GdipSetClipRectI(_graphics, r.X, r.Y, r.W, r.H, 0);
    }

    public void ResetClip()
    {
        Win32.GdipFlush(_graphics, 0);
        Win32.SelectClipRgn(_dc, IntPtr.Zero);
        Win32.GdipResetClip(_graphics);
    }

    private static IntPtr RoundPath(float x, float y, float w, float h, float radius)
    {
        float d = Math.Min(radius * 2, Math.Min(w, h));
        Win32.GdipCreatePath(FillModeWinding, out var path);
        Win32.GdipAddPathArc(path, x, y, d, d, 180, 90);
        Win32.GdipAddPathArc(path, x + w - d, y, d, d, 270, 90);
        Win32.GdipAddPathArc(path, x + w - d, y + h - d, d, d, 0, 90);
        Win32.GdipAddPathArc(path, x, y + h - d, d, d, 90, 90);
        Win32.GdipClosePathFigure(path);
        return path;
    }
}

/// <summary>A window and its window procedure. Everything that can go wrong in a message ends up in one place.</summary>
internal abstract unsafe class NativeWindow
{
    private const string ClassName = "hacBrewPackLZ.Window";
    private const string PopupClassName = "hacBrewPackLZ.Popup";

    private static readonly Dictionary<IntPtr, NativeWindow> Live = [];
    private static bool s_registered;
    [ThreadStatic] private static NativeWindow? t_creating;

    public IntPtr Handle { get; private set; }

    protected void CreateNative(string title, uint style, uint exStyle, IntPtr owner, bool popup)
    {
        Register();
        t_creating = this;
        Win32.CreateWindowExW(exStyle, popup ? PopupClassName : ClassName, title, style,
            Win32.CW_USEDEFAULT, Win32.CW_USEDEFAULT, Win32.CW_USEDEFAULT, Win32.CW_USEDEFAULT, owner, IntPtr.Zero, Win32.GetModuleHandleW(null), IntPtr.Zero);
        t_creating = null;
        if (Handle == IntPtr.Zero)
            throw new InvalidOperationException($"Couldn't create a window (error {Marshal.GetLastWin32Error()})");
    }

    protected abstract IntPtr OnMessage(uint message, IntPtr wParam, IntPtr lParam);

    private static void Register()
    {
        if (s_registered)
            return;
        s_registered = true;

        var instance = Win32.GetModuleHandleW(null);
        var large = Win32.LoadImageW(instance, Win32.AppIconResource, Win32.IMAGE_ICON, Win32.GetSystemMetricsForDpi(Win32.SM_CXICON, 96), Win32.GetSystemMetricsForDpi(Win32.SM_CXICON, 96), 0);
        var small = Win32.LoadImageW(instance, Win32.AppIconResource, Win32.IMAGE_ICON, Win32.GetSystemMetricsForDpi(Win32.SM_CXSMICON, 96), Win32.GetSystemMetricsForDpi(Win32.SM_CXSMICON, 96), 0);
        foreach (var (name, style) in new[] { (ClassName, Win32.CS_DBLCLKS), (PopupClassName, Win32.CS_DBLCLKS | Win32.CS_DROPSHADOW) })
        {
            fixed (char* classNameChars = name)
            {
                var windowClass = new WNDCLASSEXW
                {
                    Size = (uint)sizeof(WNDCLASSEXW),
                    Style = style,
                    WindowProc = (IntPtr)(delegate* unmanaged<IntPtr, uint, IntPtr, IntPtr, IntPtr>)&WindowProc,
                    Instance = instance,
                    Icon = large,
                    SmallIcon = small,
                    Cursor = Win32.LoadCursorW(IntPtr.Zero, Win32.IDC_ARROW),
                    ClassName = (IntPtr)classNameChars,
                };
                if (Win32.RegisterClassExW(ref windowClass) == 0)
                    throw new InvalidOperationException($"Couldn't register a window class (error {Marshal.GetLastWin32Error()})");
            }
        }
    }

    [UnmanagedCallersOnly]
    private static IntPtr WindowProc(IntPtr window, uint message, IntPtr wParam, IntPtr lParam)
    {
        try
        {
            if (!Live.TryGetValue(window, out var target))
            {
                target = t_creating;
                if (target is null)
                    return Win32.DefWindowProcW(window, message, wParam, lParam);
                target.Handle = window;
                Live[window] = target;
            }
            var result = target.OnMessage(message, wParam, lParam);
            if (message == Win32.WM_NCDESTROY)
                Live.Remove(window);
            return result;
        }
        catch (Exception e)
        {
            GuiProgram.Fatal(e);
            return IntPtr.Zero;
        }
    }
}

/// <summary>A picture made from a JPEG or PNG, kept at the size it's shown at.</summary>
internal sealed class Picture : IDisposable
{
    public IntPtr Handle { get; private set; }

    private Picture(IntPtr handle) => Handle = handle;

    /// <summary>Null if it isn't a picture GDI+ can read.</summary>
    public static unsafe Picture? Decode(byte[] data, int size)
    {
        IntPtr stream = Win32.SHCreateMemStream(data, (uint)data.Length);
        if (stream == IntPtr.Zero)
            return null;
        try
        {
            if (Win32.GdipCreateBitmapFromStream(stream, out IntPtr full) != 0)
                return null;
            // The full picture reads from the stream for as long as it's kept, so only the smaller copy is.
            int status = Win32.GdipGetImageThumbnail(full, (uint)size, (uint)size, out IntPtr small, IntPtr.Zero, IntPtr.Zero);
            Win32.GdipDisposeImage(full);
            return status == 0 ? new Picture(small) : null;
        }
        finally
        {
            var release = (delegate* unmanaged[Stdcall]<IntPtr, uint>)(*(*(void***)stream + 2));
            release(stream);
        }
    }

    public void Dispose()
    {
        if (Handle != IntPtr.Zero)
            Win32.GdipDisposeImage(Handle);
        Handle = IntPtr.Zero;
    }
}
