using System.Runtime.InteropServices;

namespace HacBrewPackLz;

[StructLayout(LayoutKind.Sequential)]
internal struct POINT
{
    public int X;
    public int Y;
}

[StructLayout(LayoutKind.Sequential)]
internal struct RECT
{
    public int Left;
    public int Top;
    public int Right;
    public int Bottom;
}

[StructLayout(LayoutKind.Sequential)]
internal struct MSG
{
    public IntPtr Window;
    public uint Message;
    public IntPtr WParam;
    public IntPtr LParam;
    public uint Time;
    public POINT Point;
    public uint Private;
}

[StructLayout(LayoutKind.Sequential)]
internal struct WNDCLASSEXW
{
    public uint Size;
    public uint Style;
    public IntPtr WindowProc;
    public int ClassExtra;
    public int WindowExtra;
    public IntPtr Instance;
    public IntPtr Icon;
    public IntPtr Cursor;
    public IntPtr Background;
    public IntPtr MenuName;
    public IntPtr ClassName;
    public IntPtr SmallIcon;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct PAINTSTRUCT
{
    public IntPtr Dc;
    public int Erase;
    public RECT Paint;
    public int Restore;
    public int IncUpdate;
    public fixed byte Reserved[32];
}

[StructLayout(LayoutKind.Sequential)]
internal struct TRACKMOUSEEVENT
{
    public uint Size;
    public uint Flags;
    public IntPtr Window;
    public uint HoverTime;
}

[StructLayout(LayoutKind.Sequential)]
internal struct MINMAXINFO
{
    public POINT Reserved;
    public POINT MaxSize;
    public POINT MaxPosition;
    public POINT MinTrackSize;
    public POINT MaxTrackSize;
}

[StructLayout(LayoutKind.Sequential)]
internal struct MONITORINFO
{
    public uint Size;
    public RECT Monitor;
    public RECT Work;
    public uint Flags;
}

[StructLayout(LayoutKind.Sequential)]
internal struct SIZE
{
    public int Width;
    public int Height;
}

[StructLayout(LayoutKind.Sequential)]
internal struct GdiplusStartupInput
{
    public uint Version;
    public IntPtr DebugCallback;
    public int SuppressBackgroundThread;
    public int SuppressExternalCodecs;
}

[StructLayout(LayoutKind.Sequential)]
internal struct FORMATETC
{
    public ushort Format;
    public IntPtr Device;
    public uint Aspect;
    public int Index;
    public uint Medium;
}

[StructLayout(LayoutKind.Sequential)]
internal struct STGMEDIUM
{
    public uint Medium;
    public IntPtr Data;
    public IntPtr ReleaseObject;
}

[StructLayout(LayoutKind.Sequential)]
internal struct COMDLG_FILTERSPEC
{
    public IntPtr Name;
    public IntPtr Spec;
}

[StructLayout(LayoutKind.Sequential)]
internal struct INITCOMMONCONTROLSEX
{
    public uint Size;
    public uint Classes;
}

internal static unsafe class Win32
{
    public const uint WM_DESTROY = 0x0002;
    public const uint WM_SIZE = 0x0005;
    public const uint WM_ACTIVATE = 0x0006;
    public const uint WM_SETFOCUS = 0x0007;
    public const uint WM_KILLFOCUS = 0x0008;
    public const uint WM_PAINT = 0x000F;
    public const uint WM_CLOSE = 0x0010;
    public const uint WM_ERASEBKGND = 0x0014;
    public const uint WM_SETTINGCHANGE = 0x001A;
    public const uint WM_GETMINMAXINFO = 0x0024;
    public const uint WM_SETFONT = 0x0030;
    public const uint WM_COMMAND = 0x0111;
    public const int EN_CHANGE = 0x0300;
    public const uint WM_NCDESTROY = 0x0082;
    public const uint WM_KEYDOWN = 0x0100;
    public const uint WM_CHAR = 0x0102;
    public const uint WM_CTLCOLOREDIT = 0x0133;
    public const uint WM_CTLCOLORSTATIC = 0x0138;
    public const uint WM_MOUSEMOVE = 0x0200;
    public const uint WM_LBUTTONDOWN = 0x0201;
    public const uint WM_LBUTTONUP = 0x0202;
    public const uint WM_LBUTTONDBLCLK = 0x0203;
    public const uint WM_MOUSEWHEEL = 0x020A;
    public const uint WM_CAPTURECHANGED = 0x0215;
    public const uint WM_MOUSELEAVE = 0x02A3;
    public const uint WM_DPICHANGED = 0x02E0;
    public const uint WM_APP = 0x8000;

    public const uint EM_SETSEL = 0x00B1;
    public const uint EM_SCROLLCARET = 0x00B7;
    public const uint EM_REPLACESEL = 0x00C2;
    public const uint EM_SETMARGINS = 0x00D3;
    public const uint EM_SETLIMITTEXT = 0x00C5;
    public const uint EM_SETCUEBANNER = 0x1501;

    public const uint WS_OVERLAPPEDWINDOW = 0x00CF0000;
    public const uint WS_POPUP = 0x80000000;
    public const uint WS_CHILD = 0x40000000;
    public const uint WS_CLIPCHILDREN = 0x02000000;
    public const uint WS_VSCROLL = 0x00200000;
    public const uint WS_HSCROLL = 0x00100000;
    public const uint WS_EX_TOOLWINDOW = 0x00000080;

    public const uint ES_MULTILINE = 0x0004;
    public const uint ES_AUTOVSCROLL = 0x0040;
    public const uint ES_AUTOHSCROLL = 0x0080;
    public const uint ES_READONLY = 0x0800;

    public const uint CS_DBLCLKS = 0x0008;
    public const uint CS_DROPSHADOW = 0x00020000;

    public const uint SWP_NOZORDER = 0x0004;
    public const uint SWP_NOACTIVATE = 0x0010;
    public const uint SWP_SHOWWINDOW = 0x0040;
    public const uint SWP_HIDEWINDOW = 0x0080;

    public const int SW_SHOW = 5;

    public const int GWLP_WNDPROC = -4;
    public const uint TME_LEAVE = 2;
    public const uint SRCCOPY = 0x00CC0020;
    public const int TRANSPARENT = 1;
    public const uint MB_OK = 0;
    public const uint MB_YESNOCANCEL = 3;
    public const uint MB_YESNO = 4;
    public const uint MB_ICONERROR = 0x10;
    public const uint MB_ICONWARNING = 0x30;
    public const uint MB_ICONINFORMATION = 0x40;
    public const int IDCANCEL = 2;
    public const int IDYES = 6;

    public const int VK_TAB = 0x09;
    public const int VK_RETURN = 0x0D;
    public const int VK_SHIFT = 0x10;
    public const int VK_ESCAPE = 0x1B;
    public const int VK_SPACE = 0x20;
    public const int VK_END = 0x23;
    public const int VK_HOME = 0x24;
    public const int VK_LEFT = 0x25;
    public const int VK_UP = 0x26;
    public const int VK_RIGHT = 0x27;
    public const int VK_DOWN = 0x28;
    public const int VK_DELETE = 0x2E;

    public const int CW_USEDEFAULT = unchecked((int)0x80000000);
    public const int IDC_ARROW = 32512;
    public const int AppIconResource = 32512;
    public const uint IMAGE_ICON = 1;
    public const int SM_CXICON = 11;
    public const int SM_CXSMICON = 49;
    public static readonly IntPtr HKEY_CURRENT_USER = unchecked((IntPtr)(int)0x80000001);
    public const uint RRF_RT_REG_DWORD = 0x10;
    public const int DWMWA_USE_IMMERSIVE_DARK_MODE_OLD = 19;
    public const int DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
    public const int DWMWA_CAPTION_COLOR = 35;
    public const int DWMWA_WINDOW_CORNER_PREFERENCE = 33;
    public const int CornerRoundSmall = 3;
    public const uint DT_CALCRECT = 0x0400;
    public static readonly IntPtr DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = new(-4);
    public const uint MONITOR_DEFAULTTONEAREST = 2;

    public static int Low(IntPtr value) => (short)((long)value & 0xFFFF);
    public static int High(IntPtr value) => (short)(((long)value >> 16) & 0xFFFF);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern ushort RegisterClassExW(ref WNDCLASSEXW windowClass);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateWindowExW(uint exStyle, string className, string title, uint style, int x, int y, int width, int height, IntPtr parent, IntPtr menu, IntPtr instance, IntPtr param);

    [DllImport("user32.dll")]
    public static extern IntPtr DefWindowProcW(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern IntPtr CallWindowProcW(IntPtr proc, IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern IntPtr SetWindowLongPtrW(IntPtr window, int index, IntPtr value);

    [DllImport("user32.dll")]
    public static extern bool DestroyWindow(IntPtr window);

    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr window, int command);

    [DllImport("user32.dll")]
    public static extern bool EnableWindow(IntPtr window, bool enable);

    [DllImport("user32.dll")]
    public static extern bool IsWindowEnabled(IntPtr window);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr window);

    [DllImport("user32.dll")]
    public static extern int GetMessageW(out MSG message, IntPtr window, uint min, uint max);

    [DllImport("user32.dll")]
    public static extern bool TranslateMessage(ref MSG message);

    [DllImport("user32.dll")]
    public static extern IntPtr DispatchMessageW(ref MSG message);

    [DllImport("user32.dll")]
    public static extern void PostQuitMessage(int code);

    [DllImport("user32.dll")]
    public static extern bool PostMessageW(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern IntPtr SendMessageW(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")]
    public static extern IntPtr SendMessageText(IntPtr window, uint message, IntPtr wParam, string text);

    [DllImport("user32.dll")]
    public static extern bool InvalidateRect(IntPtr window, IntPtr rect, bool erase);

    [DllImport("user32.dll")]
    public static extern IntPtr BeginPaint(IntPtr window, out PAINTSTRUCT paint);

    [DllImport("user32.dll")]
    public static extern bool EndPaint(IntPtr window, ref PAINTSTRUCT paint);

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr window, out RECT rect);

    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int width, int height, uint flags);

    [DllImport("user32.dll")]
    public static extern IntPtr LoadCursorW(IntPtr instance, IntPtr name);

    [DllImport("user32.dll")]
    public static extern IntPtr SetCapture(IntPtr window);

    [DllImport("user32.dll")]
    public static extern bool ReleaseCapture();

    [DllImport("user32.dll")]
    public static extern bool TrackMouseEvent(ref TRACKMOUSEEVENT track);

    [DllImport("user32.dll")]
    public static extern bool ScreenToClient(IntPtr window, ref POINT point);

    [DllImport("user32.dll")]
    public static extern bool ClientToScreen(IntPtr window, ref POINT point);

    [DllImport("user32.dll")]
    public static extern IntPtr SetFocus(IntPtr window);

    [DllImport("user32.dll")]
    public static extern short GetKeyState(int key);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int MessageBoxW(IntPtr owner, string text, string caption, uint type);

    [DllImport("user32.dll")]
    public static extern uint GetDpiForWindow(IntPtr window);

    [DllImport("user32.dll")]
    public static extern int GetSystemMetricsForDpi(int index, uint dpi);

    [DllImport("user32.dll")]
    public static extern bool SetProcessDpiAwarenessContext(IntPtr context);

    [DllImport("user32.dll")]
    public static extern IntPtr MonitorFromWindow(IntPtr window, uint flags);

    [DllImport("user32.dll")]
    public static extern bool GetMonitorInfoW(IntPtr monitor, ref MONITORINFO info);

    [DllImport("user32.dll")]
    public static extern IntPtr GetDC(IntPtr window);

    [DllImport("user32.dll")]
    public static extern int ReleaseDC(IntPtr window, IntPtr dc);

    [DllImport("user32.dll")]
    public static extern int FillRect(IntPtr dc, ref RECT rect, IntPtr brush);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int DrawTextW(IntPtr dc, string text, int count, ref RECT rect, uint format);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern bool SetWindowTextW(IntPtr window, string text);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowTextW(IntPtr window, char* buffer, int size);

    [DllImport("user32.dll")]
    public static extern int GetWindowTextLengthW(IntPtr window);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr LoadImageW(IntPtr instance, IntPtr name, uint type, int width, int height, uint flags);

    [DllImport("gdi32.dll")]
    public static extern IntPtr CreateCompatibleDC(IntPtr dc);

    [DllImport("gdi32.dll")]
    public static extern IntPtr CreateCompatibleBitmap(IntPtr dc, int width, int height);

    [DllImport("gdi32.dll")]
    public static extern IntPtr SelectObject(IntPtr dc, IntPtr obj);

    [DllImport("gdi32.dll")]
    public static extern bool DeleteObject(IntPtr obj);

    [DllImport("gdi32.dll")]
    public static extern bool DeleteDC(IntPtr dc);

    [DllImport("gdi32.dll")]
    public static extern bool BitBlt(IntPtr target, int x, int y, int width, int height, IntPtr source, int sourceX, int sourceY, uint operation);

    [DllImport("gdi32.dll")]
    public static extern IntPtr CreateSolidBrush(uint color);

    [DllImport("gdi32.dll")]
    public static extern uint SetTextColor(IntPtr dc, uint color);

    [DllImport("gdi32.dll")]
    public static extern uint SetBkColor(IntPtr dc, uint color);

    [DllImport("gdi32.dll")]
    public static extern int SetBkMode(IntPtr dc, int mode);

    [DllImport("gdi32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr CreateFontW(int height, int width, int escapement, int orientation, int weight, uint italic, uint underline, uint strikeOut, uint charset, uint outPrecision, uint clipPrecision, uint quality, uint pitchAndFamily, string face);

    [DllImport("gdi32.dll", CharSet = CharSet.Unicode)]
    public static extern bool GetTextExtentPoint32W(IntPtr dc, string text, int count, out SIZE size);

    [DllImport("gdi32.dll")]
    public static extern int IntersectClipRect(IntPtr dc, int left, int top, int right, int bottom);

    [DllImport("gdi32.dll")]
    public static extern int SelectClipRgn(IntPtr dc, IntPtr region);

    [DllImport("gdiplus.dll")]
    public static extern int GdiplusStartup(out IntPtr token, ref GdiplusStartupInput input, IntPtr output);

    [DllImport("gdiplus.dll")]
    public static extern int GdipCreateFromHDC(IntPtr dc, out IntPtr graphics);

    [DllImport("gdiplus.dll")]
    public static extern int GdipDeleteGraphics(IntPtr graphics);

    [DllImport("gdiplus.dll")]
    public static extern int GdipSetSmoothingMode(IntPtr graphics, int mode);

    [DllImport("gdiplus.dll")]
    public static extern int GdipSetPixelOffsetMode(IntPtr graphics, int mode);

    [DllImport("gdiplus.dll")]
    public static extern int GdipCreateSolidFill(uint argb, out IntPtr brush);

    [DllImport("gdiplus.dll")]
    public static extern int GdipDeleteBrush(IntPtr brush);

    [DllImport("gdiplus.dll")]
    public static extern int GdipCreatePen1(uint argb, float width, int unit, out IntPtr pen);

    [DllImport("gdiplus.dll")]
    public static extern int GdipDeletePen(IntPtr pen);

    [DllImport("gdiplus.dll")]
    public static extern int GdipCreatePath(int fillMode, out IntPtr path);

    [DllImport("gdiplus.dll")]
    public static extern int GdipDeletePath(IntPtr path);

    [DllImport("gdiplus.dll")]
    public static extern int GdipAddPathArc(IntPtr path, float x, float y, float width, float height, float start, float sweep);

    [DllImport("gdiplus.dll")]
    public static extern int GdipClosePathFigure(IntPtr path);

    [DllImport("gdiplus.dll")]
    public static extern int GdipFillPath(IntPtr graphics, IntPtr brush, IntPtr path);

    [DllImport("gdiplus.dll")]
    public static extern int GdipDrawPath(IntPtr graphics, IntPtr pen, IntPtr path);

    [DllImport("gdiplus.dll")]
    public static extern int GdipFillEllipse(IntPtr graphics, IntPtr brush, float x, float y, float width, float height);

    [DllImport("gdiplus.dll")]
    public static extern int GdipSetClipRectI(IntPtr graphics, int x, int y, int width, int height, int mode);

    [DllImport("gdiplus.dll")]
    public static extern int GdipResetClip(IntPtr graphics);

    [DllImport("gdiplus.dll")]
    public static extern int GdipFlush(IntPtr graphics, int intention);

    [DllImport("gdiplus.dll")]
    public static extern int GdipCreateBitmapFromStream(IntPtr stream, out IntPtr bitmap);

    [DllImport("gdiplus.dll")]
    public static extern int GdipGetImageThumbnail(IntPtr image, uint width, uint height, out IntPtr thumbnail, IntPtr callback, IntPtr data);

    [DllImport("gdiplus.dll")]
    public static extern int GdipDisposeImage(IntPtr image);

    [DllImport("gdiplus.dll")]
    public static extern int GdipDrawImageRectI(IntPtr graphics, IntPtr image, int x, int y, int width, int height);

    [DllImport("gdiplus.dll")]
    public static extern int GdipSetInterpolationMode(IntPtr graphics, int mode);

    [DllImport("shlwapi.dll")]
    public static extern IntPtr SHCreateMemStream(byte[] data, uint size);

    [DllImport("dwmapi.dll")]
    public static extern int DwmSetWindowAttribute(IntPtr window, int attribute, ref int value, int size);

    [DllImport("uxtheme.dll", CharSet = CharSet.Unicode)]
    public static extern int SetWindowTheme(IntPtr window, string? appName, string? idList);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
    public static extern int RegGetValueW(IntPtr key, string subKey, string value, uint flags, IntPtr type, out int data, ref int size);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr GetModuleHandleW(string? name);

    [DllImport("comctl32.dll")]
    public static extern bool InitCommonControlsEx(ref INITCOMMONCONTROLSEX init);

    [DllImport("ole32.dll")]
    public static extern int OleInitialize(IntPtr reserved);

    [DllImport("ole32.dll")]
    public static extern int RegisterDragDrop(IntPtr window, IntPtr target);

    [DllImport("ole32.dll")]
    public static extern void ReleaseStgMedium(ref STGMEDIUM medium);

    [DllImport("ole32.dll")]
    public static extern int CoCreateInstance(ref Guid classId, IntPtr outer, uint context, ref Guid interfaceId, out IntPtr result);

    [DllImport("ole32.dll")]
    public static extern void CoTaskMemFree(IntPtr memory);

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    public static extern uint DragQueryFileW(IntPtr drop, uint index, char* buffer, uint size);

    [DllImport("shell32.dll")]
    public static extern int SHCreateItemFromParsingName([MarshalAs(UnmanagedType.LPWStr)] string path, IntPtr context, ref Guid interfaceId, out IntPtr item);
}
