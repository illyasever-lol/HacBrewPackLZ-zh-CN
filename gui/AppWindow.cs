using System.Runtime.InteropServices;

namespace HacBrewPackLz;

/// <summary>Something drawn in a window that can take the mouse and keyboard.</summary>
internal abstract class Widget
{
    private Rect _bounds;
    private bool _visible = true;

    public AppWindow Window { get; set; } = null!;
    public bool Enabled { get; set; } = true;

    public bool Visible
    {
        get => _visible;
        set
        {
            if (_visible == value)
                return;
            _visible = value;
            VisibilityChanged();
        }
    }

    /// <summary>Set for widgets in a scrolled area, which only show and respond inside it.</summary>
    public Rect? ClipTo { get; set; }

    public Rect Bounds
    {
        get => _bounds;
        set
        {
            // v21：位置没变就不要惊动控件。压缩时状态文字变化会让主窗口每秒重排数十次，
            // 每次都触发 BoundsChanged 会让原生子窗口被反复 SetWindowPos，右侧出现闪烁。
            if (_bounds == value)
                return;
            _bounds = value;
            BoundsChanged();
        }
    }

    public virtual bool CanFocus => false;
    public bool IsFocused => Window.Focused == this;
    public bool IsHovered => Window.Hovered == this;
    public bool IsPressed => Window.Captured == this;
    public int S(int value) => Window.S(value);

    public abstract void Paint(Gfx g);

    public virtual void Attached() { }
    public virtual void BoundsChanged() { }
    public virtual void VisibilityChanged() { }
    public virtual void MouseDown(int x, int y) { }
    public virtual void MouseUp(int x, int y) { }
    public virtual void MouseMove(int x, int y) { }
    public virtual void MouseLeave() { }
    public virtual void DoubleClick(int x, int y) { }
    /// <summary>The right button let go over it, as a menu of what can be done to what's there opens on.</summary>
    public virtual void RightClick(int x, int y) { }
    public virtual bool Wheel(int delta) => false;
    public virtual bool KeyDown(int key) => false;
    public virtual void Focus() { }
    public virtual void Blur() { }
    public virtual void DpiChanged() { }
    public virtual void Destroyed() { }

    public bool Hit(int x, int y) => Visible && Enabled && Bounds.Contains(x, y) && (ClipTo is not { } clip || clip.Contains(x, y));
}

/// <summary>A window drawn with widgets on a back buffer, so nothing flickers.</summary>
internal abstract class AppWindow : NativeWindow
{
    private readonly List<Widget> _widgets = [];
    private IntPtr _bufferDc;
    private IntPtr _bufferBitmap;
    private IntPtr _bufferOld;
    private int _bufferWidth;
    private int _bufferHeight;
    private bool _trackingLeave;
    private AppWindow? _modalOwner;
    /// <summary>Something in it is moving, so a timer has it drawn again about as often as the screen shows a new picture.</summary>
    private bool _moving, _stillMoving;
    private static readonly IntPtr MotionTimer = 1;
    /// <summary>When what's shown came on, as it fades in from the background, and the picture of it that's faded in, drawn once as it came
    /// on, as drawing all of it again every moment would take longer than a moment.</summary>
    private double _fadeSince = -1;
    private IntPtr _keptDc, _keptBitmap, _keptOld;
    private int _keptWidth, _keptHeight;
    /// <summary>For tests: how long each picture took to draw while something moved, added to this file.</summary>
    private static readonly string? FrameLog = Environment.GetEnvironmentVariable("HACBREWPACKLZ_GUI_FRAMES");

    public int Dpi { get; private set; } = 96;
    public int ClientWidth { get; private set; }
    public int ClientHeight { get; private set; }
    public Widget? Hovered { get; private set; }
    public Widget? Captured { get; private set; }
    public Widget? Focused { get; private set; }
    public bool KeyboardCues { get; set; }
    public bool IsClosed { get; private set; }

    public int S(int value) => value * Dpi / 96;

    protected abstract void Layout(int width, int height);
    protected abstract void PaintWindow(Gfx g);
    protected virtual (int width, int height) MinimumSize => (480, 320);
    protected virtual bool OnClosing() => true;
    protected virtual void OnClosed() { }
    protected virtual bool OnKey(int key) => false;
    protected virtual void OnMessageFromWorker() { }
    protected virtual void OnThemeChanged() { }
    /// <summary>Each moment while something moves, before it's drawn again.</summary>
    protected virtual void OnFrame() { }

    /// <summary>Said while drawing: something drawn is on its way somewhere, so it's drawn again in a moment.</summary>
    public void StillMoving() => _stillMoving = true;

    /// <summary>What's shown now fades in from the background for a moment, as a new screen does on the Switch, once the window is on the screen.</summary>
    protected void FadeIn()
    {
        _fadeSince = Win32.IsWindowVisible(Handle) && Motion.On ? Motion.Now : -1;
        // A picture of what was shown before isn't what's coming on.
        ReleaseKept();
    }

    public T Add<T>(T widget) where T : Widget
    {
        widget.Window = this;
        _widgets.Add(widget);
        widget.Attached();
        return widget;
    }

    public void Remove(Widget widget)
    {
        if (Focused == widget)
            Focused = null;
        if (Hovered == widget)
            Hovered = null;
        if (Captured == widget)
            Captured = null;
        widget.Destroyed();
        _widgets.Remove(widget);
    }

    public void Invalidate() => Win32.InvalidateRect(Handle, IntPtr.Zero, false);

    public void Relayout()
    {
        Layout(ClientWidth, ClientHeight);
        Invalidate();
    }

    /// <summary>Asks a window for its first size, in sizes for 96 DPI, on the screen the owner is on.</summary>
    protected void CreateTopLevel(string title, int width, int height, AppWindow? owner)
    {
        CreateNative(title, Win32.WS_OVERLAPPEDWINDOW | Win32.WS_CLIPCHILDREN, 0, owner?.Handle ?? IntPtr.Zero, popup: false);
        Dpi = CurrentDpi();
        ApplyTitleBarTheme();
        CenterOn(owner, S(width), S(height));
    }

    protected void CreatePopup(AppWindow owner, uint exStyle = 0)
    {
        CreateNative("", Win32.WS_POPUP | Win32.WS_CLIPCHILDREN, Win32.WS_EX_TOOLWINDOW | exStyle, owner.Handle, popup: true);
        Dpi = owner.Dpi;
        int corners = Win32.CornerRoundSmall;
        Win32.DwmSetWindowAttribute(Handle, Win32.DWMWA_WINDOW_CORNER_PREFERENCE, ref corners, sizeof(int));
    }

    /// <summary>Keeps the owner from being used until this window closes, without a second message loop.</summary>
    public void ShowModalOver(AppWindow owner)
    {
        _modalOwner = owner;
        Win32.EnableWindow(owner.Handle, false);
        Show();
    }

    public void Show()
    {
        Win32.ShowWindow(Handle, Win32.SW_SHOW);
        Win32.SetForegroundWindow(Handle);
    }

    public void Close() => Win32.PostMessageW(Handle, Win32.WM_CLOSE, IntPtr.Zero, IntPtr.Zero);

    public void SetFocusTo(Widget? widget)
    {
        if (widget is TextField field)
        {
            Win32.SetFocus(field.Edit);
            return;
        }
        Win32.SetFocus(Handle);
        ChangeFocus(widget);
    }

    public void ChangeFocus(Widget? widget)
    {
        if (Focused == widget)
            return;
        var old = Focused;
        Focused = widget;
        old?.Blur();
        widget?.Focus();
        Invalidate();
    }

    /// <summary>How tall text comes out when wrapped to this width.</summary>
    public int TextHeight(string text, Font font, int width)
    {
        var dc = Win32.GetDC(Handle);
        var old = Win32.SelectObject(dc, Gfx.FontHandle(font, Dpi));
        var rect = new RECT { Right = width };
        Win32.DrawTextW(dc, text, text.Length, ref rect, Win32.DT_CALCRECT | (uint)(TextFlags.WordBreak | TextFlags.EditControl | TextFlags.NoPrefix));
        Win32.SelectObject(dc, old);
        Win32.ReleaseDC(Handle, dc);
        return rect.Bottom;
    }

    public int TextWidth(string text, Font font)
    {
        var dc = Win32.GetDC(Handle);
        var old = Win32.SelectObject(dc, Gfx.FontHandle(font, Dpi));
        Win32.GetTextExtentPoint32W(dc, text, text.Length, out var size);
        Win32.SelectObject(dc, old);
        Win32.ReleaseDC(Handle, dc);
        return size.Width;
    }

    public void PostFromWorker() => Win32.PostMessageW(Handle, Win32.WM_APP + 1, IntPtr.Zero, IntPtr.Zero);

    protected override IntPtr OnMessage(uint message, IntPtr wParam, IntPtr lParam)
    {
        switch (message)
        {
            case Win32.WM_ERASEBKGND:
                return 1;
            case Win32.WM_PAINT:
                Paint();
                return 0;
            case Win32.WM_SIZE:
                ClientWidth = Win32.Low(lParam) & 0xFFFF;
                ClientHeight = Win32.High(lParam) & 0xFFFF;
                if (ClientWidth > 0 && ClientHeight > 0)
                    Layout(ClientWidth, ClientHeight);
                Invalidate();
                return 0;
            case Win32.WM_GETMINMAXINFO:
                SetMinimumSize(lParam);
                return 0;
            case Win32.WM_DPICHANGED:
                HandleDpiChange(wParam, lParam);
                return 0;
            case Win32.WM_LBUTTONDOWN:
                HandleMouseDown(Win32.Low(lParam), Win32.High(lParam));
                return 0;
            case Win32.WM_LBUTTONDBLCLK:
                HandleDoubleClick(Win32.Low(lParam), Win32.High(lParam));
                return 0;
            case Win32.WM_LBUTTONUP:
                HandleMouseUp(Win32.Low(lParam), Win32.High(lParam));
                return 0;
            case Win32.WM_RBUTTONUP:
                if (WidgetAt(Win32.Low(lParam), Win32.High(lParam)) is { } clicked)
                {
                    clicked.RightClick(Win32.Low(lParam), Win32.High(lParam));
                    Invalidate();
                }
                return 0;
            case Win32.WM_MOUSEMOVE:
                HandleMouseMove(Win32.Low(lParam), Win32.High(lParam));
                return 0;
            case Win32.WM_MOUSELEAVE:
                _trackingLeave = false;
                SetHovered(null);
                return 0;
            case Win32.WM_CAPTURECHANGED:
                Captured = null;
                Invalidate();
                return 0;
            case Win32.WM_MOUSEWHEEL:
                HandleWheel(wParam, lParam);
                return 0;
            case Win32.WM_KEYDOWN:
                HandleKey((int)wParam);
                return 0;
            case Win32.WM_SETTINGCHANGE:
                Theme.Refresh();
                Motion.Refresh();
                ApplyTitleBarTheme();
                OnThemeChanged();
                Invalidate();
                return Win32.DefWindowProcW(Handle, message, wParam, lParam);
            case Win32.WM_CTLCOLOREDIT or Win32.WM_CTLCOLORSTATIC:
                Win32.SetTextColor(wParam, Theme.P.Text.Gdi);
                Win32.SetBkColor(wParam, Theme.P.Surface.Gdi);
                return Gfx.Brush(Theme.P.Surface);
            case Win32.WM_COMMAND when Win32.High(wParam) == Win32.EN_CHANGE:
                TextField.FromEdit(lParam)?.Changed?.Invoke();
                return 0;
            case Win32.WM_APP + 1:
                OnMessageFromWorker();
                return 0;
            case Win32.WM_TIMER when wParam == MotionTimer:
                OnFrame();
                Invalidate();
                return 0;
            case Win32.WM_CLOSE:
                // Whatever is open over this window goes first.
                if (Win32.IsWindowEnabled(Handle) && OnClosing())
                    Win32.DestroyWindow(Handle);
                return 0;
            case Win32.WM_DESTROY:
                IsClosed = true;
                foreach (var widget in _widgets)
                    widget.Destroyed();
                if (_modalOwner is { } owner)
                {
                    Win32.EnableWindow(owner.Handle, true);
                    Win32.SetForegroundWindow(owner.Handle);
                }
                OnClosed();
                ReleaseBuffer();
                return 0;
            default:
                return Win32.DefWindowProcW(Handle, message, wParam, lParam);
        }
    }

    private int CurrentDpi()
    {
        string? forced = Environment.GetEnvironmentVariable("HACBREWPACKLZ_GUI_DPI");
        return forced is not null && int.TryParse(forced, out int dpi) ? dpi : (int)Win32.GetDpiForWindow(Handle);
    }

    private void ApplyTitleBarTheme()
    {
        int dark = Theme.IsDark ? 1 : 0;
        if (Win32.DwmSetWindowAttribute(Handle, Win32.DWMWA_USE_IMMERSIVE_DARK_MODE, ref dark, sizeof(int)) != 0)
            Win32.DwmSetWindowAttribute(Handle, Win32.DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, ref dark, sizeof(int));
        int caption = (int)Theme.P.Background.Gdi;
        Win32.DwmSetWindowAttribute(Handle, Win32.DWMWA_CAPTION_COLOR, ref caption, sizeof(int));
    }

    private void CenterOn(AppWindow? owner, int width, int height)
    {
        var info = new MONITORINFO { Size = (uint)Marshal.SizeOf<MONITORINFO>() };
        Win32.GetMonitorInfoW(Win32.MonitorFromWindow(owner?.Handle ?? Handle, Win32.MONITOR_DEFAULTTONEAREST), ref info);
        width = Math.Min(width, info.Work.Right - info.Work.Left);
        height = Math.Min(height, info.Work.Bottom - info.Work.Top);
        int x = info.Work.Left + (info.Work.Right - info.Work.Left - width) / 2;
        int y = info.Work.Top + (info.Work.Bottom - info.Work.Top - height) / 2;
        if (owner is not null)
        {
            Win32.GetWindowRect(owner.Handle, out var o);
            x = Math.Clamp(o.Left + (o.Right - o.Left - width) / 2, info.Work.Left, info.Work.Right - width);
            y = Math.Clamp(o.Top + (o.Bottom - o.Top - height) / 2, info.Work.Top, info.Work.Bottom - height);
        }
        Win32.SetWindowPos(Handle, IntPtr.Zero, x, y, width, height, Win32.SWP_NOZORDER | Win32.SWP_NOACTIVATE);
    }

    private unsafe void SetMinimumSize(IntPtr lParam)
    {
        var info = (MINMAXINFO*)lParam;
        var (width, height) = MinimumSize;
        info->MinTrackSize.X = S(width);
        info->MinTrackSize.Y = S(height);
    }

    private unsafe void HandleDpiChange(IntPtr wParam, IntPtr lParam)
    {
        Dpi = CurrentDpi();
        var suggested = (RECT*)lParam;
        foreach (var widget in _widgets)
            widget.DpiChanged();
        Win32.SetWindowPos(Handle, IntPtr.Zero, suggested->Left, suggested->Top, suggested->Right - suggested->Left, suggested->Bottom - suggested->Top, Win32.SWP_NOZORDER | Win32.SWP_NOACTIVATE);
        Relayout();
    }

    private void Paint()
    {
        long started = FrameLog is null ? 0 : System.Diagnostics.Stopwatch.GetTimestamp();
        Win32.BeginPaint(Handle, out var paint);
        EnsureBuffer();
        _stillMoving = false;
        double fading = Motion.Along(_fadeSince, Motion.FadeTime);
        bool fromKept = fading < 1 && _keptDc != IntPtr.Zero && _keptWidth == ClientWidth && _keptHeight == ClientHeight;
        if (!fromKept)
        {
            using var g = new Gfx(_bufferDc, Dpi);
            g.Fill(new Rect(0, 0, ClientWidth, ClientHeight), Theme.P.Background);
            PaintWindow(g);
            foreach (var widget in _widgets)
            {
                if (!widget.Visible)
                    continue;
                if (widget.ClipTo is { } clip)
                    g.Clip(clip);
                widget.Paint(g);
                if (widget.ClipTo is not null)
                    g.ResetClip();
            }
            PaintOver(g);
        }
        if (fading < 1)
        {
            // What came on, drawn once and kept, shows through the background as far as it's faded in.
            if (!fromKept)
                Keep();
            using (var g = new Gfx(_bufferDc, Dpi))
                g.Fill(new Rect(0, 0, ClientWidth, ClientHeight), Theme.P.Background);
            Win32.GdiAlphaBlend(_bufferDc, 0, 0, ClientWidth, ClientHeight, _keptDc, 0, 0, ClientWidth, ClientHeight, (uint)Math.Round(255 * fading) << 16);
            StillMoving();
        }
        else if (_fadeSince >= 0)
        {
            _fadeSince = -1;
            ReleaseKept();
        }
        Win32.BitBlt(paint.Dc, 0, 0, ClientWidth, ClientHeight, _bufferDc, 0, 0, Win32.SRCCOPY);
        Win32.EndPaint(Handle, ref paint);
        if (FrameLog is not null && (_moving || _stillMoving))
            File.AppendAllText(FrameLog, $"{GetType().Name} at {Motion.Now:0.000} s took {System.Diagnostics.Stopwatch.GetElapsedTime(started).TotalMilliseconds:0.0} ms{(fading < 1 ? fromKept ? " fading" : " fading, drawn" : "")}\n");
        // Windows asks for a new picture only when something has changed, so while anything moves, a timer asks every moment.
        if (_stillMoving != _moving)
        {
            _moving = _stillMoving;
            if (_moving)
                Win32.SetTimer(Handle, MotionTimer, 10, IntPtr.Zero);
            else
                Win32.KillTimer(Handle, MotionTimer);
        }
    }

    protected virtual void PaintOver(Gfx g) { }

    private void EnsureBuffer()
    {
        if (_bufferDc != IntPtr.Zero && _bufferWidth >= ClientWidth && _bufferHeight >= ClientHeight)
            return;
        ReleaseBuffer();
        var screen = Win32.GetDC(Handle);
        _bufferDc = Win32.CreateCompatibleDC(screen);
        _bufferWidth = Math.Max(ClientWidth, 1);
        _bufferHeight = Math.Max(ClientHeight, 1);
        _bufferBitmap = Win32.CreateCompatibleBitmap(screen, _bufferWidth, _bufferHeight);
        _bufferOld = Win32.SelectObject(_bufferDc, _bufferBitmap);
        Win32.ReleaseDC(Handle, screen);
    }

    /// <summary>A copy of what's just been drawn, to fade in.</summary>
    private void Keep()
    {
        ReleaseKept();
        var screen = Win32.GetDC(Handle);
        _keptDc = Win32.CreateCompatibleDC(screen);
        _keptWidth = ClientWidth;
        _keptHeight = ClientHeight;
        _keptBitmap = Win32.CreateCompatibleBitmap(screen, Math.Max(_keptWidth, 1), Math.Max(_keptHeight, 1));
        _keptOld = Win32.SelectObject(_keptDc, _keptBitmap);
        Win32.ReleaseDC(Handle, screen);
        Win32.BitBlt(_keptDc, 0, 0, _keptWidth, _keptHeight, _bufferDc, 0, 0, Win32.SRCCOPY);
    }

    private void ReleaseKept()
    {
        if (_keptDc == IntPtr.Zero)
            return;
        Win32.SelectObject(_keptDc, _keptOld);
        Win32.DeleteObject(_keptBitmap);
        Win32.DeleteDC(_keptDc);
        _keptDc = IntPtr.Zero;
    }

    private void ReleaseBuffer()
    {
        ReleaseKept();
        if (_bufferDc == IntPtr.Zero)
            return;
        Win32.SelectObject(_bufferDc, _bufferOld);
        Win32.DeleteObject(_bufferBitmap);
        Win32.DeleteDC(_bufferDc);
        _bufferDc = IntPtr.Zero;
    }

    private Widget? WidgetAt(int x, int y)
    {
        for (int i = _widgets.Count - 1; i >= 0; i--)
        {
            if (_widgets[i].Hit(x, y))
                return _widgets[i];
        }
        return null;
    }

    private void SetHovered(Widget? widget)
    {
        if (Hovered == widget)
            return;
        Hovered?.MouseLeave();
        Hovered = widget;
        Invalidate();
    }

    private void HandleMouseDown(int x, int y)
    {
        KeyboardCues = false;
        var widget = WidgetAt(x, y);
        if (widget is { CanFocus: true })
            SetFocusTo(widget);
        else
            SetFocusTo(null);
        if (widget is null)
            return;
        Captured = widget;
        Win32.SetCapture(Handle);
        widget.MouseDown(x, y);
        Invalidate();
    }

    private void HandleDoubleClick(int x, int y)
    {
        var widget = WidgetAt(x, y);
        if (widget is null)
            return;
        widget.DoubleClick(x, y);
        Invalidate();
    }

    private void HandleMouseUp(int x, int y)
    {
        var widget = Captured;
        Captured = null;
        Win32.ReleaseCapture();
        widget?.MouseUp(x, y);
        Invalidate();
    }

    private void HandleMouseMove(int x, int y)
    {
        if (!_trackingLeave)
        {
            var track = new TRACKMOUSEEVENT { Size = (uint)Marshal.SizeOf<TRACKMOUSEEVENT>(), Flags = Win32.TME_LEAVE, Window = Handle };
            Win32.TrackMouseEvent(ref track);
            _trackingLeave = true;
        }
        if (Captured is { } captured)
        {
            captured.MouseMove(x, y);
            Invalidate();
            return;
        }
        var widget = WidgetAt(x, y);
        SetHovered(widget);
        widget?.MouseMove(x, y);
    }

    private void HandleWheel(IntPtr wParam, IntPtr lParam)
    {
        var point = new POINT { X = Win32.Low(lParam), Y = Win32.High(lParam) };
        Win32.ScreenToClient(Handle, ref point);
        int delta = Win32.High(wParam);
        for (int i = _widgets.Count - 1; i >= 0; i--)
        {
            if (_widgets[i].Hit(point.X, point.Y) && _widgets[i].Wheel(delta))
            {
                Invalidate();
                return;
            }
        }
        OnWheel(delta);
    }

    protected virtual void OnWheel(int delta) { }

    public void HandleKey(int key)
    {
        if (key == Win32.VK_TAB)
        {
            KeyboardCues = true;
            MoveFocus((Win32.GetKeyState(Win32.VK_SHIFT) & 0x8000) != 0);
            return;
        }
        if (Focused?.KeyDown(key) == true)
        {
            KeyboardCues = true;
            Invalidate();
            return;
        }
        OnKey(key);
    }

    private void MoveFocus(bool backwards)
    {
        var order = _widgets.Where(w => w.Visible && w.Enabled && w.CanFocus && (w.ClipTo is not { } clip || (w.Bounds.Y >= clip.Y && w.Bounds.Bottom <= clip.Bottom))).ToList();
        if (order.Count == 0)
            return;
        int index = Focused is null ? -1 : order.IndexOf(Focused);
        index = backwards ? (index <= 0 ? order.Count - 1 : index - 1) : (index + 1) % order.Count;
        SetFocusTo(order[index]);
        Invalidate();
    }
}
