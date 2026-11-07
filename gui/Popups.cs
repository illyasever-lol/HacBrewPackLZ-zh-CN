namespace HacBrewPackLz;

/// <summary>A small list that opens below a control, for menus and choices. It closes itself when anything else is clicked.</summary>
internal sealed class PopupList : AppWindow
{
    private readonly (string text, char icon, Action action)[] _items;
    private readonly int _chosen;
    private readonly int _width;
    private int _hover = -1;
    private bool _closing;

    public PopupList(AppWindow owner, Rect below, (string text, char icon, Action action)[] items, int chosen = -1)
    {
        _items = items;
        _chosen = chosen;
        CreatePopup(owner);
        int iconRoom = items.Any(i => i.icon != '\0') ? S(26) : 0;
        _width = Math.Max(below.W, items.Max(i => TextWidth(i.text, Fonts.Body)) + iconRoom + S(40));

        var corner = new POINT { X = below.X, Y = below.Bottom + S(4) };
        Win32.ClientToScreen(owner.Handle, ref corner);
        int height = items.Length * S(ItemHeight) + S(8);
        Win32.SetWindowPos(Handle, IntPtr.Zero, corner.X, corner.Y, _width, height, Win32.SWP_NOZORDER | Win32.SWP_NOACTIVATE);
        Win32.ShowWindow(Handle, Win32.SW_SHOW);
        Win32.SetForegroundWindow(Handle);
        Win32.SetFocus(Handle);
    }

    private const int ItemHeight = 32;

    protected override void Layout(int width, int height) { }

    protected override void PaintWindow(Gfx g)
    {
        var p = Theme.P;
        g.Fill(new Rect(0, 0, ClientWidth, ClientHeight), p.StrongBorder);
        g.Fill(new Rect(1, 1, ClientWidth - 2, ClientHeight - 2), p.Surface);
        for (int i = 0; i < _items.Length; i++)
        {
            var row = new Rect(S(4), S(4) + i * S(ItemHeight), ClientWidth - S(8), S(ItemHeight));
            if (i == _hover)
                g.FillRound(row, S(4), p.Hover);
            int x = row.X + S(10);
            if (_items[i].icon != '\0')
            {
                g.Icon(_items[i].icon, new Rect(x, row.Y, S(16), row.H), 14, p.SubText);
                x += S(26);
            }
            g.Text(_items[i].text, new Rect(x, row.Y, row.Right - x - S(8), row.H), i == _chosen ? Fonts.Strong : Fonts.Body, i == _chosen ? p.Accent : p.Text);
        }
    }

    protected override IntPtr OnMessage(uint message, IntPtr wParam, IntPtr lParam)
    {
        switch (message)
        {
            case Win32.WM_MOUSEMOVE:
                int hover = ItemAt(Win32.High(lParam));
                if (hover != _hover)
                {
                    _hover = hover;
                    Invalidate();
                }
                base.OnMessage(message, wParam, lParam);
                return 0;
            case Win32.WM_LBUTTONUP:
                int item = ItemAt(Win32.High(lParam));
                if (item >= 0)
                {
                    var action = _items[item].action;
                    Dismiss();
                    action();
                }
                return 0;
            case Win32.WM_KEYDOWN:
                HandlePopupKey((int)wParam);
                return 0;
            case Win32.WM_ACTIVATE when Win32.Low(wParam) == 0:
                Dismiss();
                return 0;
        }
        return base.OnMessage(message, wParam, lParam);
    }

    private void HandlePopupKey(int key)
    {
        switch (key)
        {
            case Win32.VK_ESCAPE:
                Dismiss();
                break;
            case Win32.VK_DOWN:
                _hover = Math.Min(_hover + 1, _items.Length - 1);
                Invalidate();
                break;
            case Win32.VK_UP:
                _hover = Math.Max(_hover - 1, 0);
                Invalidate();
                break;
            case Win32.VK_RETURN or Win32.VK_SPACE when _hover >= 0:
                var action = _items[_hover].action;
                Dismiss();
                action();
                break;
        }
    }

    private int ItemAt(int y)
    {
        int index = (y - S(4)) / S(ItemHeight);
        return y >= S(4) && index < _items.Length ? index : -1;
    }

    private void Dismiss()
    {
        if (_closing)
            return;
        _closing = true;
        Win32.DestroyWindow(Handle);
    }
}

/// <summary>Some notes below a button, each wrapped to fit, and kept inside the window that opened it. It closes when anything else is clicked.</summary>
internal sealed class NotePopup : AppWindow
{
    private const int Padding = 14;
    private const int LineHeight = 20;
    private const int Gap = 10;
    private const int Widest = 520;
    private readonly List<List<string>> _notes = [];
    private bool _closing;

    public NotePopup(AppWindow owner, Rect below, string[] notes)
    {
        CreatePopup(owner);
        int room = Math.Min(S(Widest), owner.ClientWidth - S(40)) - S(Padding) * 2 - S(26);
        int widest = 0;
        foreach (string note in notes)
        {
            var lines = Wrap(note, room);
            widest = Math.Max(widest, lines.Max(l => TextWidth(l, Fonts.Body)));
            _notes.Add(lines);
        }
        int width = widest + S(Padding) * 2 + S(26);
        int height = S(Padding) * 2 + _notes.Sum(n => n.Count) * S(LineHeight) + (_notes.Count - 1) * S(Gap);

        // Right-aligned with the button, but never past either side of the window.
        int x = Math.Clamp(below.Right - width, S(20), Math.Max(S(20), owner.ClientWidth - S(20) - width));
        var corner = new POINT { X = x, Y = below.Bottom + S(4) };
        Win32.ClientToScreen(owner.Handle, ref corner);
        Win32.SetWindowPos(Handle, IntPtr.Zero, corner.X, corner.Y, width, height, Win32.SWP_NOZORDER | Win32.SWP_NOACTIVATE);
        Win32.ShowWindow(Handle, Win32.SW_SHOW);
        Win32.SetForegroundWindow(Handle);
        Win32.SetFocus(Handle);
    }

    /// <summary>Word by word, and a word too long for a line on its own, like a long file name, gets a line of its own anyway.</summary>
    private List<string> Wrap(string text, int width)
    {
        var lines = new List<string>();
        string line = "";
        foreach (string word in text.Split(' '))
        {
            string longer = line.Length == 0 ? word : line + " " + word;
            if (line.Length > 0 && TextWidth(longer, Fonts.Body) > width)
            {
                lines.Add(line);
                line = word;
            }
            else
            {
                line = longer;
            }
        }
        lines.Add(line);
        return lines;
    }

    protected override void Layout(int width, int height) { }

    protected override void PaintWindow(Gfx g)
    {
        var p = Theme.P;
        g.Fill(new Rect(0, 0, ClientWidth, ClientHeight), p.StrongBorder);
        g.Fill(new Rect(1, 1, ClientWidth - 2, ClientHeight - 2), p.Surface);
        int y = S(Padding);
        foreach (var lines in _notes)
        {
            g.Icon(Glyph.Info, new Rect(S(Padding), y, S(16), S(LineHeight)), 13, p.Faint);
            foreach (string line in lines)
            {
                g.Text(line, new Rect(S(Padding) + S(26), y, ClientWidth - S(Padding) * 2 - S(26), S(LineHeight)), Fonts.Body, p.Text);
                y += S(LineHeight);
            }
            y += S(Gap);
        }
    }

    protected override IntPtr OnMessage(uint message, IntPtr wParam, IntPtr lParam)
    {
        switch (message)
        {
            case Win32.WM_LBUTTONUP:
            case Win32.WM_KEYDOWN:
            case Win32.WM_ACTIVATE when Win32.Low(wParam) == 0:
                if (!_closing)
                {
                    _closing = true;
                    Win32.DestroyWindow(Handle);
                }
                return 0;
        }
        return base.OnMessage(message, wParam, lParam);
    }
}
