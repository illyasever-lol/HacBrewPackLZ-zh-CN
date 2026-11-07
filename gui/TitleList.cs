namespace HacBrewPackLz;

/// <summary>The list on the main window: a title per row, then anything that couldn't be used, and then how many are hidden. What there is to
/// know about a title is behind the info button at the end of its row.</summary>
internal sealed class TitleList(Action<TitleRow> toggle, Action<bool> selectAll, Action<TitleRow> remove, Action<InputIssue> removeIssue, Action<string> showOutput,
    Action<TitleRow, Rect> showNotes, Action<bool> showCompressed) : Widget
{
    private const int TickColumn = 40;
    private const int TickSize = 18;
    private const int IconSize = 40;
    private const int HeaderHeight = 34;
    private const int RowHeight = 56;
    private const int NoteHeight = 34;
    private const int ButtonSize = 28;
    private const int ColumnNow = 96;
    private const int ColumnAfter = 104;
    private const int ColumnSaved = 68;
    private const int ColumnActions = 104;

    private enum Part
    {
        None,
        Row,
        Tick,
        TickAll,
        Remove,
        Output,
        Info,
        Hidden,
        ScrollBar,
    }

    private readonly ScrollMath _scroll = new();
    private TitleRow[] _rows = [];
    private InputIssue[] _issues = [];
    private string[] _notes = [];
    /// <summary>Which of the notes says how many compressed games are hidden or shown, with a link at its end to change that.</summary>
    private int _hiddenNote = -1;
    private string _hiddenLink = "";
    private bool _busy;
    /// <summary>What's being read, while files are, so an empty list says so rather than asking for files.</summary>
    private string? _reading;
    private SendSnapshot? _send;
    private readonly Dictionary<string, (byte[] data, int size, Picture? picture)> _pictures = [];
    private int _hoverIndex = -1;
    private Part _hoverPart;
    private int _selected = -1;

    /// <summary>Space kept free at the bottom of the card for the buttons that add more.</summary>
    public int FooterHeight { get; set; }

    /// <summary>Files are being held over the window.</summary>
    public bool DropHover { get; set; }

    public bool IsEmpty => _rows.Length == 0 && _issues.Length == 0 && _notes.Length == 0;

    public override bool CanFocus => !IsEmpty;

    private int ItemCount => _rows.Length + _issues.Length + _notes.Length;

    public void SetData(SessionSnapshot state)
    {
        _rows = state.Rows;
        _issues = state.Issues;
        _notes = state.Notes;
        _hiddenNote = -1;
        _hiddenLink = "";
        var hidden = new List<string>();
        int compressed = state.HiddenCompressed > 0 ? state.HiddenCompressed : state.ShownCompressed;
        if (compressed > 0 && !state.IsBusy)
        {
            hidden.Add($"{compressed} 个已压缩的游戏{(state.HiddenCompressed > 0 ? "已隐藏" : "已显示")}。 ");
            _hiddenLink = state.HiddenCompressed > 0 ? "显示它们" : "隐藏它们";
        }
        if (state.HiddenSmall > 0)
            hidden.Add($"已隐藏 {state.HiddenSmall} 个 1 MB 以下的游戏（依设置）");
        if (hidden.Count > 0 && state.Rows.Length + state.Issues.Length > 0)
        {
            if (_hiddenLink.Length > 0)
                _hiddenNote = _notes.Length;
            _notes = [.. _notes, .. hidden];
        }
        _busy = state.IsBusy;
        _reading = state.Activity == SessionActivity.Reading ? state.ReadingText : null;
        _send = state.Activity == SessionActivity.Sending ? state.Send : null;
        _selected = Math.Min(_selected, _rows.Length - 1);

        // Pictures of what's no longer on the list go.
        foreach (string key in _pictures.Keys.Where(k => !_rows.Any(r => r.Key == k)).ToList())
        {
            _pictures[key].picture?.Dispose();
            _pictures.Remove(key);
        }
        Measure();
    }

    /// <summary>A title's icon at the size it's shown, made the first time it's needed.</summary>
    private Picture? PictureOf(TitleRow row)
    {
        if (row.Icon is not { } data)
            return null;
        int size = S(IconSize);
        if (_pictures.TryGetValue(row.Key, out var known) && known.data == data && known.size == size)
            return known.picture;
        known.picture?.Dispose();
        var picture = Picture.Decode(data, size);
        _pictures[row.Key] = (data, size, picture);
        return picture;
    }

    public override void BoundsChanged() => Measure();

    private Rect ListArea => new(Bounds.X, Bounds.Y + S(HeaderHeight), Bounds.W, Bounds.H - S(HeaderHeight) - S(FooterHeight));

    private int ItemHeight(int index) => index >= _rows.Length + _issues.Length ? S(NoteHeight) : S(RowHeight);

    private int ItemTop(int index)
    {
        int rowHeight = S(RowHeight);
        int top = Math.Min(index, _rows.Length + _issues.Length) * rowHeight;
        return top + Math.Max(index - _rows.Length - _issues.Length, 0) * S(NoteHeight);
    }

    private void Measure() => _scroll.Measure(ItemTop(ItemCount), Math.Max(ListArea.H, 1));

    public override void Paint(Gfx g)
    {
        var p = Theme.P;
        g.FillRound(Bounds, S(10), p.Surface);
        if (IsEmpty)
        {
            PaintDropZone(g);
            return;
        }

        var (columnNow, columnAfter, columnSaved) = Columns();
        if (_rows.Length > 0)
            PaintTick(g, TickBox(new Rect(Bounds.X, Bounds.Y, Bounds.W, S(HeaderHeight))), _rows.All(r => r.Selected), _rows.Any(r => r.Selected), _hoverPart == Part.TickAll && !_busy);
        g.Text("名称", new Rect(Bounds.X + S(TickColumn) + S(6) + S(IconSize) + S(12), Bounds.Y, S(120), S(HeaderHeight)), Fonts.Small, p.Faint);
        var header = new Rect(0, Bounds.Y, 0, S(HeaderHeight));
        if (columnNow > 0)
            g.Text("当前", header with { X = columnNow, W = S(ColumnNow) }, Fonts.Small, p.Faint, TextFlags.Line | TextFlags.Right);
        g.Text("之后", header with { X = columnAfter, W = S(ColumnAfter) }, Fonts.Small, p.Faint, TextFlags.Line | TextFlags.Right);
        if (columnSaved > 0)
            g.Text("节省", header with { X = columnSaved, W = S(ColumnSaved) }, Fonts.Small, p.Faint, TextFlags.Line | TextFlags.Right);
        g.Fill(new Rect(Bounds.X, Bounds.Y + S(HeaderHeight) - 1, Bounds.W, 1), p.Border);

        var area = ListArea;
        g.Clip(area);
        for (int i = 0; i < ItemCount; i++)
        {
            int top = area.Y + ItemTop(i) - _scroll.Offset;
            int height = ItemHeight(i);
            if (top + height <= area.Y || top >= area.Bottom)
                continue;
            var row = new Rect(Bounds.X, top, Bounds.W, height);
            if (i < _rows.Length)
                PaintRow(g, _rows[i], i, row, columnNow, columnAfter, columnSaved);
            else if (i < _rows.Length + _issues.Length)
                PaintIssue(g, _issues[i - _rows.Length], i, row);
            else
                PaintNote(g, i, row);
        }
        g.ResetClip();

        _scroll.Paint(g, new Rect(Bounds.Right - S(8), area.Y + S(4), S(4), area.H - S(8)), _hoverPart == Part.ScrollBar || _scroll.Dragging);
        if (FooterHeight > 0)
            g.Fill(new Rect(Bounds.X, area.Bottom, Bounds.W, 1), p.Border);

        if (DropHover)
        {
            g.StrokeRound(Bounds, S(10), p.Accent, S(2));
        }
        else if (IsFocused && Window.KeyboardCues)
        {
            g.StrokeRound(Bounds, S(10), p.Accent, S(2));
        }
    }

    private void PaintDropZone(Gfx g)
    {
        var p = Theme.P;
        var zone = Bounds.Inset(S(10));
        g.StrokeRound(zone, S(8), DropHover ? p.Accent : p.StrongBorder, DropHover ? S(2) : 1);
        if (DropHover)
            g.FillRound(zone.Inset(S(2)), S(7), p.Hover);

        int middle = Bounds.Y + Bounds.H / 2;
        if (_reading is not null && !DropHover)
        {
            PaintSpinner(g, Bounds.X + Bounds.W / 2, middle - S(74));
            g.Text("正在读取游戏", new Rect(Bounds.X, middle - S(48), Bounds.W, S(28)), Fonts.Hero, p.Text, TextFlags.Line | TextFlags.Center);
            g.Text(_reading + "…", new Rect(Bounds.X + S(20), middle - S(18), Bounds.W - S(40), S(22)), Fonts.Body, p.SubText, TextFlags.Line | TextFlags.Center);
            return;
        }
        g.Icon(Glyph.Download, new Rect(Bounds.X, middle - S(96), Bounds.W, S(44)), 34, DropHover ? p.Accent : p.Faint);
        g.Text("将游戏文件或文件夹拖到此处", new Rect(Bounds.X, middle - S(48), Bounds.W, S(28)), Fonts.Hero, p.Text, TextFlags.Line | TextFlags.Center);
        g.Text("支持 NSP、NSZ、XCI、XCZ 文件，以及含 exefs、romfs、control 的游戏文件夹。原始文件不会被修改。", new Rect(Bounds.X + S(20), middle - S(18), Bounds.W - S(40), S(22)), Fonts.Body, p.SubText, TextFlags.Line | TextFlags.Center);
    }

    /// <summary>A ring of dots with a bright one going round, kept turning by the window's timer.</summary>
    private void PaintSpinner(Gfx g, int centerX, int centerY)
    {
        var p = Theme.P;
        const int Dots = 8;
        int lit = (int)(Environment.TickCount64 / 110 % Dots);
        int radius = S(14), dot = S(6);
        for (int i = 0; i < Dots; i++)
        {
            double angle = Math.PI * 2 * i / Dots - Math.PI / 2;
            int x = centerX + (int)Math.Round(Math.Cos(angle) * radius) - dot / 2;
            int y = centerY + (int)Math.Round(Math.Sin(angle) * radius) - dot / 2;
            int behind = (lit - i + Dots) % Dots;
            g.FillRound(new Rect(x, y, dot, dot), dot / 2, behind == 0 ? p.Accent : behind < 3 ? p.SubText : p.StrongBorder);
        }
    }

    private void PaintRow(Gfx g, TitleRow row, int index, Rect r, int columnNow, int columnAfter, int columnSaved)
    {
        var p = Theme.P;
        bool hovered = _hoverIndex == index;
        if (hovered && !_busy)
            g.Fill(r, p.Hover);
        else if (index == _selected && IsFocused && Window.KeyboardCues)
            g.Fill(r, p.Hover);

        int textRight = (columnNow > 0 ? columnNow : columnAfter) - S(8);
        var icon = new Rect(r.X + S(TickColumn) + S(6), r.Y + (r.H - S(IconSize)) / 2, S(IconSize), S(IconSize));
        int textLeft = icon.Right + S(12);
        var name = new Rect(textLeft, r.Y + S(8), textRight - textLeft, S(22));
        var detail = name with { Y = r.Y + S(29), H = S(18) };
        PaintTick(g, TickBox(r), row.Selected, row.Selected, hovered && _hoverPart == Part.Tick && !_busy);
        if (PictureOf(row) is { } picture)
        {
            g.Image(picture, icon);
        }
        else
        {
            g.FillRound(icon, S(6), p.Hover);
            g.Icon(row.IsFolder ? Glyph.Folder : Glyph.Document, icon, 16, p.Faint);
        }
        g.Text(row.Name, name, Fonts.Strong, row.Selected ? p.Text : p.Faint);
        bool failed = row.State == RowState.Failed && row.Failure is not null;
        g.Text(failed ? row.Failure! : Capitalized(row.DetailWithEstimate), detail, Fonts.Small, failed ? p.Bad : row.BelowMinimum is not null ? p.Warn : p.SubText);

        var numbers = new Rect(0, r.Y, 0, r.H);
        if (columnNow > 0)
            g.Text(ProgressReporter.Format(row.InputBytes), numbers with { X = columnNow, W = S(ColumnNow) }, Fonts.Body, p.SubText, TextFlags.Line | TextFlags.Right);

        var (after, afterColor, saved, savedColor) = Result(row);
        g.Text(after, numbers with { X = columnAfter, W = S(ColumnAfter) }, Fonts.Body, row.Selected ? afterColor : p.Faint, TextFlags.Line | TextFlags.Right);
        if (columnSaved > 0)
            g.Text(saved, numbers with { X = columnSaved, W = S(ColumnSaved) }, Fonts.Strong, row.Selected ? savedColor : p.Faint, TextFlags.Line | TextFlags.Right);

        // The info button is always there when there's something to say, so it's seen without looking for it.
        if (row.Notes.Length > 0)
            PaintRowButton(g, InfoButton(r), Glyph.Info, hovered && _hoverPart == Part.Info);
        if (hovered && !_busy)
        {
            if (row.OutputPath is not null)
                PaintRowButton(g, OutputButton(r), Glyph.FolderOpen, _hoverPart == Part.Output);
            PaintRowButton(g, RemoveButton(r), Glyph.Close, _hoverPart == Part.Remove);
        }
        g.Fill(new Rect(r.X + S(16), r.Bottom - 1, r.W - S(32), 1), p.Border);
    }

    private void PaintIssue(Gfx g, InputIssue issue, int index, Rect r)
    {
        var p = Theme.P;
        bool hovered = _hoverIndex == index;
        if (hovered && !_busy)
            g.Fill(r, p.Hover);
        g.Icon(Glyph.Warning, new Rect(r.X + S(14), r.Y, S(20), r.H), 15, p.Warn);
        var text = new Rect(r.X + S(44), r.Y + S(6), r.W - S(44) - S(ColumnActions), r.H - S(12));
        g.Text(issue.Message, text, Fonts.Small, p.Warn, TextFlags.Wrapped);
        if (hovered && !_busy)
            PaintRowButton(g, RemoveButton(r), Glyph.Close, _hoverPart == Part.Remove);
        g.Fill(new Rect(r.X + S(16), r.Bottom - 1, r.W - S(32), 1), p.Border);
    }

    private void PaintNote(Gfx g, int index, Rect r)
    {
        var p = Theme.P;
        bool link = index - _rows.Length - _issues.Length == _hiddenNote;
        g.Icon(Glyph.Info, new Rect(r.X + S(14), r.Y, S(20), r.H), 13, p.Faint);
        var text = new Rect(r.X + S(44), r.Y, r.W - S(60), r.H);
        g.Text(_notes[index - _rows.Length - _issues.Length], text, Fonts.Small, p.Faint);
        if (!link)
            return;
        // Only the link can be clicked, and it keeps its size under the mouse: it's underlined instead.
        var at = LinkArea(index, r);
        bool hovered = _hoverIndex == index && _hoverPart == Part.Hidden && !_busy;
        g.Text(_hiddenLink, at, Fonts.Small, p.Accent);
        if (hovered)
            g.Fill(new Rect(at.X, at.Y + at.H / 2 + S(8), at.W, 1), p.Accent);
    }

    private Rect LinkArea(int index, Rect r)
    {
        int left = r.X + S(44) + Window.TextWidth(_notes[index - _rows.Length - _issues.Length], Fonts.Small);
        return new Rect(left, r.Y, Window.TextWidth(_hiddenLink, Fonts.Small), r.H);
    }

    private Rect TickBox(Rect row) => new(row.X + (S(TickColumn) + S(12) - S(TickSize)) / 2, row.Y + (row.H - S(TickSize)) / 2, S(TickSize), S(TickSize));

    private void PaintTick(Gfx g, Rect box, bool ticked, bool partly, bool hovered)
    {
        var p = Theme.P;
        if (ticked || partly)
        {
            g.FillRound(box, S(4), p.Accent);
            if (ticked)
                g.Icon(Glyph.Check, box, 11, p.OnAccent);
            else
                g.Fill(new Rect(box.X + S(4), box.Y + box.H / 2 - S(1), box.W - S(8), S(2)), p.OnAccent);
        }
        else
        {
            g.StrokeRound(box, S(4), hovered ? p.Accent : p.StrongBorder);
        }
    }

    private void PaintRowButton(Gfx g, Rect r, char glyph, bool hovered)
    {
        var p = Theme.P;
        if (hovered)
            g.FillRound(r, S(4), p.Pressed);
        g.Icon(glyph, r, 12, hovered ? p.Text : p.SubText);
    }

    private static string Capitalized(string text) => text.Length > 0 ? char.ToUpperInvariant(text[0]) + text[1..] : text;

    private (string after, Rgb afterColor, string saved, Rgb savedColor) Result(TitleRow row)
    {
        var p = Theme.P;
        // While sending, what the Switch has read of each one is what matters.
        if (_send is { } send && send.Of(row.Files) is { length: > 0 } sent)
        {
            return sent.read >= sent.length
                ? ("已发送", p.Good, "", p.Text)
                : ($"Sending {sent.read * 100 / sent.length}%", p.Accent, "", p.Text);
        }
        switch (row.State)
        {
            case RowState.Working:
                return ("处理中…", p.Accent, "", p.Text);
            case RowState.Failed:
                return ("失败", p.Bad, "", p.Text);
            case RowState.Done when row.DoneAs == BatchMode.Extract:
                return ("已解包", p.Good, "", p.Text);
            case RowState.Done:
                return (ProgressReporter.Format(row.OutputBytes), p.Good, Percent(row.InputBytes, row.OutputBytes), p.Good);
        }
        if (row.Estimate is not { } estimate)
            return ("", p.Text, "", p.Text);
        string approximately = estimate.MarginBytes > 0 ? "~" : "";
        return (approximately + ProgressReporter.Format(estimate.OutputBytes), p.Text, Percent(estimate.InputBytes, estimate.OutputBytes), estimate.SavedBytes >= 0 ? p.Good : p.Warn);
    }

    public static string Percent(long before, long after)
    {
        if (before <= 0)
            return "";
        double saved = (before - after) * 100.0 / before;
        return saved >= 0 ? $"{saved:0}%" : $"+{-saved:0}%";
    }

    /// <summary>Columns from the right. In a small window, Saved goes first and then Now, so the title always has room.</summary>
    private (int now, int after, int saved) Columns()
    {
        int right = Bounds.Right - S(ColumnActions);
        bool showSaved = Bounds.W >= S(480);
        bool showNow = Bounds.W >= S(600);
        int saved = showSaved ? right - S(ColumnSaved) : 0;
        int after = (showSaved ? saved : right) - S(ColumnAfter);
        int now = showNow ? after - S(ColumnNow) : 0;
        return (now, after, saved);
    }

    private Rect RemoveButton(Rect row) => new(row.Right - S(16) - S(ButtonSize), row.Y + (row.H - S(ButtonSize)) / 2, S(ButtonSize), S(ButtonSize));

    private Rect OutputButton(Rect row) => RemoveButton(row) with { X = row.Right - S(16) - S(ButtonSize) * 2 - S(2) };

    private Rect InfoButton(Rect row) => RemoveButton(row) with { X = row.Right - S(16) - S(ButtonSize) * 3 - S(4) };

    private (int index, Part part) PartAt(int x, int y)
    {
        var area = ListArea;
        if (_rows.Length > 0 && y >= Bounds.Y && y < Bounds.Y + S(HeaderHeight) && x >= Bounds.X && x < Bounds.X + S(TickColumn) + S(12))
            return (-1, Part.TickAll);
        if (IsEmpty || !area.Contains(x, y))
            return (-1, Part.None);
        if (_scroll.Max > 0 && x >= Bounds.Right - S(14))
            return (-1, Part.ScrollBar);

        int relative = y - area.Y + _scroll.Offset;
        for (int i = 0; i < ItemCount; i++)
        {
            int top = ItemTop(i);
            if (relative < top || relative >= top + ItemHeight(i))
                continue;
            var row = new Rect(Bounds.X, area.Y + top - _scroll.Offset, Bounds.W, ItemHeight(i));
            if (i < _rows.Length + _issues.Length && RemoveButton(row).Contains(x, y))
                return (i, Part.Remove);
            if (i < _rows.Length && _rows[i].OutputPath is not null && OutputButton(row).Contains(x, y))
                return (i, Part.Output);
            if (i < _rows.Length && _rows[i].Notes.Length > 0 && InfoButton(row).Contains(x, y))
                return (i, Part.Info);
            if (i == _rows.Length + _issues.Length + _hiddenNote && _hiddenNote >= 0 && LinkArea(i, row).Contains(x, y))
                return (i, Part.Hidden);
            if (i < _rows.Length && x < Bounds.X + S(TickColumn) + S(12))
                return (i, Part.Tick);
            return (i, Part.Row);
        }
        return (-1, Part.None);
    }

    public override void MouseMove(int x, int y)
    {
        if (_scroll.Dragging)
        {
            _scroll.Drag(new Rect(Bounds.Right - S(8), ListArea.Y + S(4), S(4), ListArea.H - S(8)), y);
            Window.Invalidate();
            return;
        }
        var (index, part) = PartAt(x, y);
        if (index != _hoverIndex || part != _hoverPart)
        {
            _hoverIndex = index;
            _hoverPart = part;
            Window.Invalidate();
        }
    }

    public override void MouseLeave()
    {
        _hoverIndex = -1;
        _hoverPart = Part.None;
    }

    public override void MouseDown(int x, int y)
    {
        var (index, part) = PartAt(x, y);
        if (part == Part.ScrollBar)
        {
            _scroll.Begin(new Rect(Bounds.Right - S(8), ListArea.Y + S(4), S(4), ListArea.H - S(8)), x, y, S(14));
        }
        else if (index >= 0 && index < _rows.Length)
        {
            _selected = index;
        }
    }

    public override void MouseUp(int x, int y)
    {
        if (_scroll.Dragging)
        {
            _scroll.End();
            return;
        }
        if (_busy)
            return;
        var (index, part) = PartAt(x, y);
        if (part == Part.TickAll)
        {
            selectAll(!_rows.All(r => r.Selected));
        }
        else if (part == Part.Tick)
        {
            toggle(_rows[index]);
        }
        else if (part == Part.Remove)
        {
            if (index < _rows.Length)
                remove(_rows[index]);
            else
                removeIssue(_issues[index - _rows.Length]);
        }
        else if (part == Part.Output && _rows[index].OutputPath is { } path)
        {
            showOutput(path);
        }
        else if (part == Part.Info)
        {
            var area = ListArea;
            var row = new Rect(Bounds.X, area.Y + ItemTop(index) - _scroll.Offset, Bounds.W, ItemHeight(index));
            showNotes(_rows[index], InfoButton(row));
        }
        else if (part == Part.Hidden)
        {
            showCompressed(_hiddenLink == "显示它们");
        }
    }

    public override void DoubleClick(int x, int y)
    {
        var (index, part) = PartAt(x, y);
        if (part == Part.Row && index < _rows.Length && _rows[index].OutputPath is { } path)
            showOutput(path);
    }

    public override bool Wheel(int delta)
    {
        if (_scroll.Max == 0)
            return false;
        _scroll.ScrollBy(-delta * S(RowHeight) / 120);
        return true;
    }

    public override bool KeyDown(int key)
    {
        switch (key)
        {
            case Win32.VK_UP:
                Select(Math.Max(_selected - 1, 0));
                return true;
            case Win32.VK_DOWN:
                Select(Math.Min(_selected + 1, _rows.Length - 1));
                return true;
            case Win32.VK_HOME:
                Select(0);
                return true;
            case Win32.VK_END:
                Select(_rows.Length - 1);
                return true;
            case Win32.VK_DELETE when _selected >= 0 && _selected < _rows.Length && !_busy:
                remove(_rows[_selected]);
                return true;
            case Win32.VK_SPACE when _selected >= 0 && _selected < _rows.Length && !_busy:
                toggle(_rows[_selected]);
                return true;
            case Win32.VK_RETURN when _selected >= 0 && _selected < _rows.Length && _rows[_selected].OutputPath is { } path:
                showOutput(path);
                return true;
        }
        return false;
    }

    private void Select(int index)
    {
        if (_rows.Length == 0)
            return;
        _selected = Math.Clamp(index, 0, _rows.Length - 1);
        _scroll.ScrollInto(ItemTop(_selected), ItemHeight(_selected));
    }
}
