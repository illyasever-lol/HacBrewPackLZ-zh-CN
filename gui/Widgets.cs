using System.Runtime.InteropServices;

namespace HacBrewPackLz;

internal enum ButtonStyle
{
    Primary,
    Secondary,
    Ghost,
}

internal sealed class Button(string text, ButtonStyle style, Action click, char icon = '\0') : Widget
{
    public string Text { get; set; } = text;
    public ButtonStyle Style { get; set; } = style;
    public char Icon { get; set; } = icon;
    public bool AlignLeft { get; set; }

    /// <summary>With this, the right end of the button is a part of its own, with an arrow, that opens a menu of what else it can do. The
    /// menu goes below the button.</summary>
    public Action<Rect>? Menu { get; set; }

    /// <summary>Whether the button's own action can be used, when the menu still can.</summary>
    public bool MainEnabled { get; set; } = true;

    private const int MenuPart = 30;
    private bool _overMenu;

    public override bool CanFocus => true;

    public int PreferredWidth()
    {
        int text = Text.Length > 0 ? Window.TextWidth(Text, Fonts.Body) : 0;
        int icon = Icon != '\0' ? S(16) + (Text.Length > 0 ? S(8) : 0) : 0;
        return text + icon + (Text.Length > 0 ? S(32) : S(20)) + (Menu is not null ? S(MenuPart) : 0);
    }

    private Rect MenuArea => new(Bounds.Right - S(MenuPart), Bounds.Y, S(MenuPart), Bounds.H);

    public override void Paint(Gfx g)
    {
        var p = Theme.P;
        bool mainUsable = Enabled && MainEnabled;
        var (background, foreground) = Style switch
        {
            ButtonStyle.Primary when !mainUsable => (p.Pressed, p.Faint),
            ButtonStyle.Primary => (IsPressed ? p.AccentPressed : IsHovered ? p.AccentHover : p.Accent, p.OnAccent),
            ButtonStyle.Secondary => (IsPressed ? p.Pressed : IsHovered ? p.Hover : p.Surface, Enabled ? p.Text : p.Faint),
            _ => (IsPressed ? p.Pressed : IsHovered ? p.Hover : p.Background, Enabled ? p.Text : p.Faint),
        };

        bool flat = Style == ButtonStyle.Ghost && !IsHovered && !IsPressed;
        if (!flat)
            g.FillRound(Bounds, S(6), background);
        if (Style == ButtonStyle.Secondary)
            g.StrokeRound(Bounds, S(6), p.Border);
        if (IsFocused && Window.KeyboardCues)
            g.StrokeRound(Bounds, S(6), p.Accent, S(2));

        var main = Bounds;
        if (Menu is not null)
        {
            var menu = MenuArea;
            main = Bounds with { W = Bounds.W - menu.W };
            bool primary = Style == ButtonStyle.Primary && mainUsable;
            if (IsHovered && _overMenu && Enabled)
                g.FillRound(menu.Inset(S(2)), S(4), primary ? p.AccentPressed : p.Pressed);
            g.Fill(new Rect(menu.X, Bounds.Y + S(8), 1, Bounds.H - S(16)), primary ? p.OnAccent : p.StrongBorder);
            g.Icon(Glyph.ChevronDown, menu, 10, Enabled ? (primary ? p.OnAccent : p.Text) : p.Faint);
        }

        int textWidth = Text.Length > 0 ? Window.TextWidth(Text, Fonts.Body) : 0;
        int iconWidth = Icon != '\0' ? S(16) : 0;
        int gap = iconWidth > 0 && textWidth > 0 ? S(8) : 0;
        int x = AlignLeft ? main.X + S(14) : main.X + (main.W - textWidth - iconWidth - gap) / 2;
        if (Icon != '\0')
        {
            g.Icon(Icon, new Rect(x, Bounds.Y, iconWidth, Bounds.H), 14, foreground);
            x += iconWidth + gap;
        }
        g.Text(Text, new Rect(x, Bounds.Y, textWidth + S(2), Bounds.H), Fonts.Body, foreground);
    }

    public override void MouseMove(int x, int y)
    {
        bool over = Menu is not null && MenuArea.Contains(x, y);
        if (over != _overMenu)
        {
            _overMenu = over;
            Window.Invalidate();
        }
    }

    public override void MouseUp(int x, int y)
    {
        if (!Bounds.Contains(x, y) || !Enabled)
            return;
        if (Menu is { } menu && MenuArea.Contains(x, y))
            menu(Bounds);
        else if (MainEnabled)
            click();
    }

    public override bool KeyDown(int key)
    {
        if (!Enabled)
            return false;
        // Down opens the menu, as it does on a split button anywhere else in Windows.
        if (key == Win32.VK_DOWN && Menu is { } menu)
        {
            menu(Bounds);
            return true;
        }
        if (key is not (Win32.VK_RETURN or Win32.VK_SPACE) || !MainEnabled)
            return false;
        click();
        return true;
    }
}

internal sealed class ToggleSwitch(Action<bool> changed) : Widget
{
    public bool On { get; set; }

    public override bool CanFocus => true;

    public override void Paint(Gfx g)
    {
        var p = Theme.P;
        int height = S(20);
        int width = S(40);
        var track = new Rect(Bounds.X, Bounds.Y + (Bounds.H - height) / 2, width, height);
        if (On)
            g.FillRound(track, height / 2f, IsHovered ? p.AccentHover : p.Accent);
        else
        {
            g.FillRound(track, height / 2f, IsHovered ? p.Hover : p.Surface);
            g.StrokeRound(track, height / 2f, p.StrongBorder);
        }
        if (IsFocused && Window.KeyboardCues)
            g.StrokeRound(track.Inset(-S(2)), height / 2f + S(2), p.Accent, S(2));
        int knob = S(12);
        g.FillCircle(On ? track.Right - S(4) - knob : track.X + S(4), track.Y + (height - knob) / 2f, knob, On ? p.OnAccent : p.SubText);
        g.Text(On ? "开" : "关", new Rect(track.Right + S(10), Bounds.Y, S(60), Bounds.H), Fonts.Body, p.SubText);
    }

    public override void MouseUp(int x, int y)
    {
        if (Bounds.Contains(x, y))
            Flip();
    }

    public override bool KeyDown(int key)
    {
        if (key is not (Win32.VK_SPACE or Win32.VK_RETURN))
            return false;
        Flip();
        return true;
    }

    private void Flip()
    {
        On = !On;
        changed(On);
    }
}

internal sealed class Slider(int min, int max, Action<int> changed) : Widget
{
    public int Value { get; set; }

    public override bool CanFocus => true;

    public override void Paint(Gfx g)
    {
        var p = Theme.P;
        int labelWidth = S(36);
        int trackHeight = S(4);
        var track = new Rect(Bounds.X + S(8), Bounds.Y + (Bounds.H - trackHeight) / 2, Bounds.W - labelWidth - S(16), trackHeight);
        g.FillRound(track, trackHeight / 2f, p.Border);
        int x = track.X + (Value - min) * track.W / Math.Max(max - min, 1);
        g.FillRound(new Rect(track.X, track.Y, x - track.X, trackHeight), trackHeight / 2f, p.Accent);
        int knob = S(16);
        g.FillCircle(x - knob / 2f, Bounds.Y + (Bounds.H - knob) / 2f, knob, IsPressed ? p.AccentPressed : IsHovered ? p.AccentHover : p.Accent);
        if (IsFocused && Window.KeyboardCues)
            g.StrokeRound(new Rect(x - knob / 2 - S(3), Bounds.Y + (Bounds.H - knob) / 2 - S(3), knob + S(6), knob + S(6)), knob, p.Accent, S(2));
        g.Text(Value.ToString(), new Rect(Bounds.Right - labelWidth, Bounds.Y, labelWidth, Bounds.H), Fonts.Strong, p.Text, TextFlags.Line | TextFlags.Right);
    }

    public override void MouseDown(int x, int y) => SetFromMouse(x);
    public override void MouseMove(int x, int y)
    {
        if (IsPressed)
            SetFromMouse(x);
    }

    public override bool KeyDown(int key)
    {
        int next = key switch
        {
            Win32.VK_LEFT or Win32.VK_DOWN => Value - 1,
            Win32.VK_RIGHT or Win32.VK_UP => Value + 1,
            Win32.VK_HOME => min,
            Win32.VK_END => max,
            _ => int.MinValue,
        };
        if (next == int.MinValue)
            return false;
        Set(next);
        return true;
    }

    private void SetFromMouse(int x)
    {
        int left = Bounds.X + S(8);
        int width = Bounds.W - S(36) - S(16);
        Set(min + (int)Math.Round((double)(x - left) / Math.Max(width, 1) * (max - min)));
    }

    private void Set(int value)
    {
        value = Math.Clamp(value, min, max);
        if (value == Value)
            return;
        Value = value;
        changed(value);
        Window.Invalidate();
    }
}

internal sealed class ProgressLine : Widget
{
    public double Fraction { get; set; }

    /// <summary>There's no saying how far along it is, so a piece of the line slides along it instead, which shows it's still going.
    /// Whoever sets this repaints it often.</summary>
    public bool Sliding { get; set; }

    public override void Paint(Gfx g)
    {
        var p = Theme.P;
        g.Fill(Bounds, p.Border);
        if (Sliding)
        {
            const int Period = 1400;
            int piece = Bounds.W / 4;
            double along = Environment.TickCount64 % Period / (double)Period;
            int x = Bounds.X - piece + (int)((Bounds.W + piece) * along);
            int left = Math.Max(x, Bounds.X), right = Math.Min(x + piece, Bounds.Right);
            if (right > left)
                g.Fill(new Rect(left, Bounds.Y, right - left, Bounds.H), p.Accent);
            return;
        }
        int width = (int)(Bounds.W * Math.Clamp(Fraction, 0, 1));
        g.Fill(new Rect(Bounds.X, Bounds.Y, width, Bounds.H), p.Accent);
    }
}

/// <summary>A native edit control inside a drawn frame, so typing, selecting and pasting work the way they should.</summary>
internal sealed unsafe class TextField(string placeholder) : Widget
{
    private static readonly Dictionary<IntPtr, (IntPtr proc, TextField field)> Edits = [];

    private string _shadow = "";
    public IntPtr Edit { get; private set; }
    public bool HasError { get; set; }

    /// <summary>The text is finished with: focus moved away or Enter was pressed.</summary>
    public Action? Committed { get; set; }

    /// <summary>The text changed, while it's still being typed.</summary>
    public Action? Changed { get; set; }

    public static TextField? FromEdit(IntPtr edit) => Edits.TryGetValue(edit, out var entry) ? entry.field : null;

    public override bool CanFocus => true;

    public string Value
    {
        get
        {
            if (Edit == IntPtr.Zero)
                return _shadow;
            int size = Math.Min(Win32.GetWindowTextLengthW(Edit), 32766) + 1;
            var buffer = stackalloc char[size];
            int read = Win32.GetWindowTextW(Edit, buffer, size);
            return new string(buffer, 0, read);
        }
        set
        {
            _shadow = value;
            if (Edit != IntPtr.Zero)
                Win32.SetWindowTextW(Edit, value);
        }
    }

    public override void Attached()
    {
        Edit = Win32.CreateWindowExW(0, "EDIT", _shadow, Win32.WS_CHILD | Win32.ES_AUTOHSCROLL, 0, 0, 10, 10, Window.Handle, IntPtr.Zero, Win32.GetModuleHandleW(null), IntPtr.Zero);
        Win32.SendMessageText(Edit, Win32.EM_SETCUEBANNER, 1, placeholder);
        var original = Win32.SetWindowLongPtrW(Edit, Win32.GWLP_WNDPROC, (IntPtr)(delegate* unmanaged<IntPtr, uint, IntPtr, IntPtr, IntPtr>)&EditProc);
        Edits[Edit] = (original, this);
        ApplyFont();
    }

    public override void Destroyed()
    {
        if (Edit == IntPtr.Zero)
            return;
        _shadow = Value;
        Win32.DestroyWindow(Edit);
        Edits.Remove(Edit);
        Edit = IntPtr.Zero;
    }

    public override void DpiChanged() => ApplyFont();
    public override void BoundsChanged() => Place();
    public override void VisibilityChanged() => Place();

    public void Place()
    {
        if (Edit == IntPtr.Zero)
            return;
        var inner = Bounds.Inset(S(10), 0);
        int line = S(20);
        bool showing = Visible && Enabled && (ClipTo is not { } clip || (Bounds.Y >= clip.Y && Bounds.Bottom <= clip.Bottom));
        Win32.SetWindowPos(Edit, IntPtr.Zero, inner.X, Bounds.Y + (Bounds.H - line) / 2, inner.W, line,
            Win32.SWP_NOZORDER | Win32.SWP_NOACTIVATE | (showing ? Win32.SWP_SHOWWINDOW : Win32.SWP_HIDEWINDOW));
    }

    public override void Paint(Gfx g)
    {
        var p = Theme.P;
        g.FillRound(Bounds, S(6), p.Surface);
        g.StrokeRound(Bounds, S(6), HasError ? p.Bad : IsFocused ? p.Accent : p.Border, IsFocused || HasError ? S(2) : 1);

        // Scrolled partly out of view, the edit control is hidden, so its text is drawn here until it's back.
        if (ClipTo is { } clip && (Bounds.Y < clip.Y || Bounds.Bottom > clip.Bottom))
        {
            string value = Value;
            g.Text(value.Length > 0 ? value : placeholder, Bounds.Inset(S(10), 0), Fonts.Body, value.Length > 0 ? p.Text : p.Faint);
        }
    }

    private void ApplyFont()
    {
        Win32.SendMessageW(Edit, Win32.WM_SETFONT, Gfx.FontHandle(Fonts.Body, Window.Dpi), 1);
        Win32.SendMessageW(Edit, Win32.EM_SETMARGINS, 3, IntPtr.Zero);
    }

    [UnmanagedCallersOnly]
    private static IntPtr EditProc(IntPtr edit, uint message, IntPtr wParam, IntPtr lParam)
    {
        try
        {
            if (!Edits.TryGetValue(edit, out var entry))
                return Win32.DefWindowProcW(edit, message, wParam, lParam);
            var (original, field) = entry;
            switch (message)
            {
                case Win32.WM_SETFOCUS:
                    field.Window.ChangeFocus(field);
                    break;
                case Win32.WM_KILLFOCUS:
                    field.Committed?.Invoke();
                    break;
                case Win32.WM_CHAR when (int)wParam is Win32.VK_TAB or Win32.VK_RETURN or Win32.VK_ESCAPE:
                    return IntPtr.Zero;
                case Win32.WM_KEYDOWN when (int)wParam is Win32.VK_TAB or Win32.VK_ESCAPE:
                    field.Window.HandleKey((int)wParam);
                    return IntPtr.Zero;
                case Win32.WM_KEYDOWN when (int)wParam == Win32.VK_RETURN:
                    field.Committed?.Invoke();
                    field.Window.HandleKey((int)wParam);
                    return IntPtr.Zero;
            }
            return Win32.CallWindowProcW(original, edit, message, wParam, lParam);
        }
        catch (Exception e)
        {
            GuiProgram.Fatal(e);
            return IntPtr.Zero;
        }
    }
}

/// <summary>Read-only text that can be selected and copied, for the details of a job.</summary>
internal sealed class LogView : Widget
{
    private IntPtr _edit;
    private int _shown;

    public override void Attached()
    {
        // Without ES_AUTOHSCROLL a multiline EDIT word-wraps, and re-wraps every line
        // on each width change: dragging the window crawled once a long transfer had
        // filled the log. Long lines get a horizontal scrollbar instead.
        _edit = Win32.CreateWindowExW(0, "EDIT", "", Win32.WS_CHILD | Win32.WS_VSCROLL | Win32.WS_HSCROLL | Win32.ES_MULTILINE | Win32.ES_READONLY | Win32.ES_AUTOVSCROLL | Win32.ES_AUTOHSCROLL,
            0, 0, 10, 10, Window.Handle, IntPtr.Zero, Win32.GetModuleHandleW(null), IntPtr.Zero);
        Win32.SendMessageW(_edit, Win32.EM_SETLIMITTEXT, 0x400000, IntPtr.Zero);
        ApplyTheme();
        ApplyFont();
    }

    public void ApplyTheme() => Win32.SetWindowTheme(_edit, Theme.IsDark ? "DarkMode_Explorer" : "资源管理器", null);

    public override void DpiChanged() => ApplyFont();
    public override void BoundsChanged() => Place();
    public override void VisibilityChanged() => Place();
    public override void Destroyed() => Win32.DestroyWindow(_edit);

    public void SetLines(LogLine[] lines)
    {
        if (lines.Length < _shown || _shown == 0)
        {
            Win32.SetWindowTextW(_edit, "");
            _shown = 0;
        }
        if (lines.Length == _shown)
            return;
        string text = string.Concat(lines.Skip(_shown).Select(l => l.Text + "\r\n"));
        _shown = lines.Length;
        Win32.SendMessageW(_edit, Win32.EM_SETSEL, -1, -1);
        Win32.SendMessageText(_edit, Win32.EM_REPLACESEL, 0, text);
        Win32.SendMessageW(_edit, Win32.EM_SCROLLCARET, IntPtr.Zero, IntPtr.Zero);
    }

    public override void Paint(Gfx g)
    {
        var p = Theme.P;
        g.FillRound(Bounds, S(8), p.Surface);
        g.StrokeRound(Bounds, S(8), p.Border);
    }

    private void Place()
    {
        if (_edit == IntPtr.Zero)
            return;
        var inner = Bounds.Inset(S(10), S(8));
        Win32.SetWindowPos(_edit, IntPtr.Zero, inner.X, inner.Y, inner.W, inner.H,
            Win32.SWP_NOZORDER | Win32.SWP_NOACTIVATE | (Visible ? Win32.SWP_SHOWWINDOW : Win32.SWP_HIDEWINDOW));
    }

    private void ApplyFont() => Win32.SendMessageW(_edit, Win32.WM_SETFONT, Gfx.FontHandle(Fonts.Small, Window.Dpi), 1);
}

internal sealed class Dropdown(string[] choices, Action<string> changed) : Widget
{
    public string Value { get; set; } = choices.Length > 0 ? choices[0] : "";

    public override bool CanFocus => true;

    public override void Paint(Gfx g)
    {
        var p = Theme.P;
        g.FillRound(Bounds, S(6), IsHovered ? p.Hover : p.Surface);
        g.StrokeRound(Bounds, S(6), IsFocused && Window.KeyboardCues ? p.Accent : p.Border, IsFocused && Window.KeyboardCues ? S(2) : 1);
        g.Text(Value, new Rect(Bounds.X + S(12), Bounds.Y, Bounds.W - S(40), Bounds.H), Fonts.Body, p.Text);
        g.Icon(Glyph.ChevronDown, new Rect(Bounds.Right - S(30), Bounds.Y, S(24), Bounds.H), 10, p.SubText);
    }

    public override void MouseUp(int x, int y)
    {
        if (Bounds.Contains(x, y))
            Open();
    }

    public override bool KeyDown(int key)
    {
        if (key is Win32.VK_RETURN or Win32.VK_SPACE)
        {
            Open();
            return true;
        }
        int index = Array.IndexOf(choices, Value);
        int next = key == Win32.VK_DOWN ? index + 1 : key == Win32.VK_UP ? index - 1 : -1;
        if (key is not (Win32.VK_DOWN or Win32.VK_UP) || next < 0 || next >= choices.Length)
            return false;
        Choose(choices[next]);
        return true;
    }

    private void Open()
    {
        var items = choices.Select(c => (c, '\0', (Action)(() => Choose(c)))).ToArray();
        _ = new PopupList(Window, Bounds, items, Array.IndexOf(choices, Value));
    }

    private void Choose(string value)
    {
        Value = value;
        changed(value);
        Window.Invalidate();
    }
}

/// <summary>The thin scroll bar of a drawn list: where it is, how it looks, and dragging it.</summary>
internal sealed class ScrollMath
{
    private int _dragStart = -1;
    private int _dragOffset;

    public int Offset { get; private set; }
    public int Content { get; private set; }
    public int View { get; private set; }
    public int Max => Math.Max(Content - View, 0);

    public void Measure(int content, int view)
    {
        Content = content;
        View = view;
        Offset = Math.Clamp(Offset, 0, Max);
    }

    public void ScrollBy(int delta) => Offset = Math.Clamp(Offset + delta, 0, Max);

    public void ScrollInto(int top, int height)
    {
        if (top < Offset)
            Offset = top;
        else if (top + height > Offset + View)
            Offset = top + height - View;
        Offset = Math.Clamp(Offset, 0, Max);
    }

    public void Paint(Gfx g, Rect area, bool active)
    {
        if (Max == 0)
            return;
        var thumb = Thumb(area);
        g.FillRound(thumb, thumb.W / 2f, active ? Theme.P.Faint : Theme.P.StrongBorder);
    }

    public bool Begin(Rect area, int x, int y, int grabWidth)
    {
        if (Max == 0 || x < area.Right - grabWidth)
            return false;
        var thumb = Thumb(area);
        if (y < thumb.Y || y >= thumb.Bottom)
            Offset = Math.Clamp((int)((long)(y - area.Y - thumb.H / 2) * Max / Math.Max(area.H - thumb.H, 1)), 0, Max);
        _dragStart = y;
        _dragOffset = Offset;
        return true;
    }

    public bool Dragging => _dragStart >= 0;

    public void Drag(Rect area, int y)
    {
        var thumb = Thumb(area);
        Offset = Math.Clamp(_dragOffset + (int)((long)(y - _dragStart) * Max / Math.Max(area.H - thumb.H, 1)), 0, Max);
    }

    public void End() => _dragStart = -1;

    private Rect Thumb(Rect area)
    {
        int height = Math.Max((int)((long)area.H * View / Math.Max(Content, 1)), area.W * 4);
        int y = area.Y + (int)((long)(area.H - height) * Offset / Math.Max(Max, 1));
        return new Rect(area.Right - area.W, y, area.W, height);
    }
}
