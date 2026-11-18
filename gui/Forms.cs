namespace HacBrewPackLz;

internal interface IFormStore
{
    string Get(string flag);

    /// <summary>Returns what's wrong with the value, leaving the old one in place, or null if it was taken.</summary>
    string? Set(OptionSpec option, string value);
}

/// <summary>Options that are kept between runs and used by the session.</summary>
internal sealed class SettingsStore(Session session) : IFormStore
{
    public string Get(string flag) => session.Settings.Get(flag);

    public string? Set(OptionSpec option, string value)
    {
        string old = session.Settings.Get(option.Flag);
        if (old == value)
            return null;
        session.Settings.Set(option.Flag, value);
        try
        {
            session.Settings.ToOptions(option.Use);
        }
        catch (ToolException e)
        {
            session.Settings.Set(option.Flag, old);
            return e.Message;
        }
        session.Settings.Save();
        session.OptionChanged(option.Flag);
        if (option.Flag == "--keyset")
            session.ReloadKeys();
        return null;
    }
}

/// <param name="运行">Does the work and closes the window, or returns what's still missing and leaves it open.</param>
internal sealed record FormAction(string Text, ButtonStyle Style, Func<string?> Run);

internal sealed class FoundFile(string path, long size)
{
    public string Path { get; } = path;
    public long Size { get; } = size;
    public bool Chosen { get; set; }
}

internal sealed class FoundList(Action changed) : Widget
{
    private const int RowHeight = 48;

    private readonly ScrollMath _scroll = new();
    private int _hover = -1;
    private int _focusRow;

    public List<FoundFile> Files { get; private set; } = [];

    public override bool CanFocus => true;

    public void SetFiles(List<FoundFile> files)
    {
        Files = files;
        _focusRow = Math.Clamp(_focusRow, 0, Math.Max(files.Count - 1, 0));
        Measure();
    }

    public override void BoundsChanged() => Measure();

    private void Measure() => _scroll.Measure(Files.Count * S(RowHeight), Math.Max(Bounds.H, 1));

    public override void Paint(Gfx g)
    {
        var p = Theme.P;
        g.FillRound(Bounds, S(10), p.Surface);
        if (IsFocused && Window.KeyboardCues)
            g.StrokeRound(Bounds, S(10), p.Accent, S(2));
        if (Files.Count == 0)
        {
            g.Text("没有可显示的游戏文件", Bounds, Fonts.Body, p.Faint, TextFlags.Line | TextFlags.Center);
            return;
        }

        g.Clip(Bounds);
        int first = _scroll.Offset / S(RowHeight);
        int last = Math.Min((_scroll.Offset + Bounds.H) / S(RowHeight) + 1, Files.Count);
        for (int i = first; i < last; i++)
        {
            var file = Files[i];
            var row = new Rect(Bounds.X, Bounds.Y + i * S(RowHeight) - _scroll.Offset, Bounds.W, S(RowHeight));
            if (i == _hover || (i == _focusRow && IsFocused && Window.KeyboardCues))
                g.Fill(row, p.Hover);
            var box = new Rect(row.X + S(16), row.Y + (row.H - S(18)) / 2, S(18), S(18));
            if (file.Chosen)
            {
                g.FillRound(box, S(4), p.Accent);
                g.Icon(Glyph.Check, box, 11, p.OnAccent);
            }
            else
            {
                g.StrokeRound(box, S(4), p.StrongBorder);
            }

            int sizeWidth = S(90);
            int textX = box.Right + S(12);
            int textWidth = row.Right - S(16) - sizeWidth - textX;
            g.Text(Path.GetFileName(file.Path), new Rect(textX, row.Y + S(6), textWidth, S(20)), Fonts.Body, p.Text);
            g.Text(Path.GetDirectoryName(file.Path) ?? "", new Rect(textX, row.Y + S(26), textWidth, S(16)), Fonts.Small, p.SubText, TextFlags.Path);
            g.Text(ProgressReporter.Format(file.Size), new Rect(row.Right - S(16) - sizeWidth, row.Y, sizeWidth, row.H), Fonts.Body, p.SubText, TextFlags.Line | TextFlags.Right);
            g.Fill(new Rect(row.X + S(16), row.Bottom - 1, row.W - S(32), 1), p.Border);
        }
        g.ResetClip();
        _scroll.Paint(g, new Rect(Bounds.Right - S(8), Bounds.Y + S(4), S(4), Bounds.H - S(8)), _scroll.Dragging);
    }

    private int RowAt(int y)
    {
        int index = (y - Bounds.Y + _scroll.Offset) / S(RowHeight);
        return index >= 0 && index < Files.Count && y >= Bounds.Y ? index : -1;
    }

    public override void MouseMove(int x, int y)
    {
        if (_scroll.Dragging)
        {
            _scroll.Drag(new Rect(Bounds.Right - S(8), Bounds.Y + S(4), S(4), Bounds.H - S(8)), y);
            return;
        }
        int hover = RowAt(y);
        if (hover != _hover)
        {
            _hover = hover;
            Window.Invalidate();
        }
    }

    public override void MouseLeave() => _hover = -1;

    public override void MouseDown(int x, int y) =>
        _scroll.Begin(new Rect(Bounds.Right - S(8), Bounds.Y + S(4), S(4), Bounds.H - S(8)), x, y, S(14));

    public override void MouseUp(int x, int y)
    {
        if (_scroll.Dragging)
        {
            _scroll.End();
            return;
        }
        if (Bounds.Contains(x, y) && RowAt(y) is >= 0 and var row)
            Toggle(row);
    }

    public override void DoubleClick(int x, int y)
    {
        if (RowAt(y) is >= 0 and var row)
            Toggle(row);
    }

    public override bool Wheel(int delta)
    {
        _scroll.ScrollBy(-delta * S(RowHeight) / 120);
        return true;
    }

    public override bool KeyDown(int key)
    {
        switch (key)
        {
            case Win32.VK_UP:
                _focusRow = Math.Max(_focusRow - 1, 0);
                break;
            case Win32.VK_DOWN:
                _focusRow = Math.Min(_focusRow + 1, Math.Max(Files.Count - 1, 0));
                break;
            case Win32.VK_SPACE when Files.Count > 0:
                Toggle(_focusRow);
                break;
            default:
                return false;
        }
        _scroll.ScrollInto(_focusRow * S(RowHeight), S(RowHeight));
        return true;
    }

    private void Toggle(int row)
    {
        Files[row].Chosen = !Files[row].Chosen;
        _focusRow = row;
        changed();
        Window.Invalidate();
    }
}
