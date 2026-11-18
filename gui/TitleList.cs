namespace HacBrewPackLz;

/// <summary>The list on the main window: a title per row, then anything that couldn't be used, and then the notes. Along the top, what it shows:
/// what's to do, what's done or everything, as on the Switch. A job has a view of its own: only its games, how each is going and then what came
/// of it, with what it's doing along the top. Everything about a game, and all that can be done to it, is its Details, behind the info button
/// at the end of its row, a double click or Enter; a right click has what can be done to it.</summary>
internal sealed class TitleList(Action<TitleRow> toggle, Action<bool> selectAll, Action<TitleRow> remove, Action<InputIssue> removeIssue, Action<string> showOutput,
    Action<TitleRow> openDetails, Action<TitleRow, Rect> gameMenu, Action<GamesShown> setView) : Widget
{
    private const int TickColumn = 40;
    private const int TickSize = 18;
    private const int IconSize = 40;
    private const int HeaderHeight = 42;
    private const int RowHeight = 56;
    private const int NoteHeight = 34;
    private const int ButtonSize = 28;
    private const int ColumnNow = 96;
    private const int ColumnAfter = 96;
    // Wide enough for "至少节省" to stand as far from "处理后" as the other headings do from each other.
    private const int ColumnSaved = 110;
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
        View,
        ScrollBar,
    }

    private readonly ScrollMath _scroll = new();
    private TitleRow[] _rows = [];
    private InputIssue[] _issues = [];
    private string[] _notes = [];
    private GamesShown _view;
    /// <summary>The views there are, and how many games each has: with compression off there's no Done.</summary>
    private GamesShown[] _views = [GamesShown.ToDo, GamesShown.Done, GamesShown.Everything];

    /// <summary>Compression is off, which changes what Done holds: games with nothing for a repack to gain.</summary>
    public bool Repacking { get; set; }
    private readonly int[] _counts = new int[3];
    private int _everything;
    private int _done;
    /// <summary>A job's own view while it's up, and the step the game being done is on.</summary>
    private JobSnapshot? _job;
    private ProgressSnapshot? _step;
    private bool _busy;
    private bool _measuring;
    /// <summary>What's being read, while files are, so an empty list says so rather than asking for files.</summary>
    private string? _reading;
    private SendSnapshot? _send;
    private readonly Dictionary<string, (byte[] data, int size, Picture? picture)> _pictures = [];
    private int _hoverIndex = -1;
    private Part _hoverPart;
    private int _selected = -1;
    /// <summary>The game the pointer was on when the list was changed for another, and how far down the list's area it was, to find it there
    /// again. None if nothing was selected.</summary>
    private string? _anchor;
    private int _anchorOnScreen;

    /// <summary>Space kept free at the bottom of the card for the buttons that add more.</summary>
    public int FooterHeight { get; set; }

    /// <summary>Files are being held over the window.</summary>
    public bool DropHover { get; set; }

    /// <summary>Nothing at all, in any view: what's asked for then is files.</summary>
    public bool IsEmpty => _everything == 0 && _issues.Length == 0 && _notes.Length == 0 && _job is null;

    /// <summary>v19：任务跑完后结果视图仍在，此时也允许按 X 移除；跑的过程中不允许。</summary>
    private bool CanRemove => _job is null || !_job.Running;

    public override bool CanFocus => !IsEmpty;

    private int ItemCount => _rows.Length + _issues.Length + _notes.Length;

    public void SetData(SessionSnapshot state)
    {
        // Another view, or a job's own view starting or ending, is another list. The game the pointer's on goes with it, as on the Switch, or
        // in a view with nothing in it, the one it was on before, so going through that view doesn't lose it.
        bool otherList = state.View != _view || (state.Job is null) != (_job is null);
        if (otherList && _rows.Length > 0)
        {
            _anchor = _selected >= 0 ? _rows[_selected].Key : null;
            _anchorOnScreen = _selected >= 0 ? ItemTop(_selected) - _scroll.Offset : 0;
        }
        _rows = state.Rows;
        _issues = state.Issues;
        // Below a job's games, once it's over, what to know about what it made, wrapped as the notes are.
        _notes = state.Job is { } job ? job.Said : state.Notes;
        _view = state.View;
        _views = state.ViewsOffered;
        for (int i = 0; i < _views.Length; i++)
            _counts[i] = state.CountOf(_views[i]);
        _everything = state.EverythingCount;
        _done = state.DoneCount;
        _job = state.Job;
        _step = state.Progress;
        _busy = state.IsBusy;
        _measuring = state.Activity == SessionActivity.Measuring;
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

        // If the new list has the game, it stays where it was on the screen. Otherwise the list starts at its top, with the pointer there if
        // it was anywhere: where a game this list doesn't have was says nothing about where anything in it is.
        if (otherList)
        {
            int found = _anchor is null ? -1 : Array.FindIndex(_rows, r => r.Key == _anchor);
            _selected = found >= 0 ? found : _anchor is not null && _rows.Length > 0 ? 0 : -1;
            _scroll.ScrollBy((found >= 0 ? ItemTop(found) - _anchorOnScreen : 0) - _scroll.Offset);
        }
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

    /// <summary>How tall each problem and note below the titles is: as tall as its text wrapped to the width there is, so none of it is cut short.</summary>
    private int[] _tailHeights = [];

    private int ItemHeight(int index)
    {
        if (index < _rows.Length)
            return S(RowHeight);
        int tail = index - _rows.Length;
        return tail < _tailHeights.Length ? _tailHeights[tail] : tail < _issues.Length ? S(RowHeight) : S(NoteHeight);
    }

    private int ItemTop(int index)
    {
        int top = Math.Min(index, _rows.Length) * S(RowHeight);
        for (int i = _rows.Length; i < index; i++)
            top += ItemHeight(i);
        return top;
    }

    private Rect IssueText(Rect r) => new(r.X + S(44), r.Y + S(6), r.W - S(44) - S(ColumnActions), r.H - S(12));

    private Rect NoteText(Rect r) => new(r.X + S(44), r.Y, r.W - S(60), r.H);

    private void Measure()
    {
        _tailHeights = new int[_issues.Length + _notes.Length];
        if (Window is not null && Bounds.W > 0)
        {
            var probe = new Rect(Bounds.X, 0, Bounds.W, 0);
            for (int i = 0; i < _issues.Length; i++)
                _tailHeights[i] = Math.Max(S(RowHeight), Window.TextHeight(_issues[i].Message, Fonts.Small, IssueText(probe).W) + S(12));
            // A note with a link stays on one line, as it's short: only the others can need more.
            for (int i = 0; i < _notes.Length; i++)
                _tailHeights[_issues.Length + i] = Math.Max(S(NoteHeight), Window.TextHeight(_notes[i], Fonts.Small, NoteText(probe).W) + S(16));
        }
        _scroll.Measure(ItemTop(ItemCount), Math.Max(ListArea.H, 1));
    }

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
        if (_job is { } job)
        {
            PaintJobHeading(g, job, (columnNow > 0 ? columnNow : columnAfter) - S(12));
        }
        else
        {
            if (_rows.Length > 0)
                PaintTick(g, TickBox(new Rect(Bounds.X, Bounds.Y, Bounds.W, S(HeaderHeight))), _rows.All(r => r.Selected), _rows.Any(r => r.Selected), _hoverPart == Part.TickAll && !_busy);
            // What's shown, with how many games each view has, as one control over the game column where a column would only say "名称":
            // the one shown is filled in, as on the Switch, so it doesn't read as one more column heading, and each can be clicked.
            var control = ViewsControl();
            g.FillRound(control, control.H / 2f, Theme.IsDark ? p.Background : p.Hover);
            g.StrokeRound(control, control.H / 2f, p.Border);
            // The one shown, filled in, sliding to it from the one before, as on the Switch.
            int showing = Array.IndexOf(_views, _view);
            if (showing >= 0)
            {
                double slid = Math.Clamp(_viewShown.Towards(showing, Window), 0, _views.Length - 1);
                int from = (int)slid, to = Math.Min(from + 1, _views.Length - 1);
                Rect a = ViewArea(from), b = ViewArea(to);
                var on = a with { X = a.X + (int)Math.Round((b.X - a.X) * (slid - from)), W = a.W + (int)Math.Round((b.W - a.W) * (slid - from)) };
                g.FillRound(on, on.H / 2f, Theme.IsDark ? p.Pressed : Rgb.Hex(0xFAFBFC));
            }
            for (int i = 0; i < _views.Length; i++)
            {
                var at = ViewArea(i);
                bool shown = _views[i] == _view;
                bool hovered = _hoverPart == Part.View && _hoverIndex == i && (!_busy || _measuring);
                g.Text(ViewLabel(i), at, shown ? Fonts.Strong : Fonts.Body, shown || hovered ? p.Text : p.SubText, TextFlags.Line | TextFlags.Center);
            }
        }
        var header = new Rect(0, Bounds.Y, 0, S(HeaderHeight));
        // On Done: what each was, what it takes now and what was freed, as on the Switch.
        if (columnNow > 0)
            g.Text(OnDone ? "原始" : "当前", header with { X = columnNow, W = S(ColumnNow) }, Fonts.Small, p.Faint, TextFlags.Line | TextFlags.Right);
        // Extracting and checking change no game's size, so what each came to has no heading over it.
        bool sizeOnly = _job is { Kind: SessionActivity.Extracting or SessionActivity.Verifying };
        if (!sizeOnly)
            g.Text(OnDone ? "当前" : "处理后", header with { X = columnAfter, W = S(ColumnAfter) }, Fonts.Small, p.Faint, TextFlags.Line | TextFlags.Right);
        // Where what's saved is an estimate, it's what each is sure to save, and the column says so.
        if (columnSaved > 0 && !sizeOnly)
            g.Text(OnDone ? "已释放" : _rows.Any(r => r.State != RowState.Done && r.Estimate is { AtLeast: true }) ? "至少节省" : "已节省", header with { X = columnSaved, W = S(ColumnSaved) },
                Fonts.Small, p.Faint, TextFlags.Line | TextFlags.Right);
        g.Fill(new Rect(Bounds.X, Bounds.Y + S(HeaderHeight) - 1, Bounds.W, 1), p.Border);

        var area = ListArea;
        if (ItemCount == 0)
        {
            // This view has nothing in it, but another does: which, and how to get there.
            int middle = area.Y + area.H / 2;
            bool done = _view == GamesShown.Done;
            string title = done ? "这里还没有已完成的项目" : "此处内容已全部处理";
            string detail = done
                ? Repacking ? "重打包完成的游戏会出现在这里，没有可并入更新补丁的游戏也会。"
                    : "压缩完成的游戏会出现在这里，添加时已是压缩格式的游戏也会。"
                : Repacking ? $"{_done} 个游戏没有可并入的更新补丁，重打包无收益，已归入「已完成」。"
                    : $"{_done} 个游戏已压缩，归入「已完成」。";
            g.Icon(done ? Glyph.Info : Glyph.Check, new Rect(area.X, middle - S(70), area.W, S(40)), 28, done ? p.Faint : p.Good);
            g.Text(title, new Rect(area.X, middle - S(26), area.W, S(28)), Fonts.Hero, p.Text, TextFlags.Line | TextFlags.Center);
            g.Text(detail, new Rect(area.X + S(40), middle + S(4), area.W - S(80), S(66)), Fonts.Body, p.SubText, TextFlags.Wrapped | TextFlags.Center);
        }
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
        g.Text("把游戏拖到这里进行压缩、解包或检查", new Rect(Bounds.X, middle - S(48), Bounds.W, S(28)), Fonts.Hero, p.Text, TextFlags.Line | TextFlags.Center);
        g.Text("支持 NSP、NSZ、XCI、XCZ，以及含 exefs、romfs、control 的游戏文件夹。原始文件不会被修改。", new Rect(Bounds.X + S(20), middle - S(18), Bounds.W - S(40), S(22)), Fonts.Body, p.SubText, TextFlags.Line | TextFlags.Center);
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
        bool working = _job is not null && row.State == RowState.Working;
        if (hovered && !_busy)
            g.Fill(r, p.Hover);
        else if (index == _selected && IsFocused && Window.KeyboardCues)
            g.Fill(r, p.Hover);
        else if (working)
            g.Fill(r, p.Hover);

        var name = NameArea(r, columnNow, columnAfter);
        var icon = new Rect(r.X + S(TickColumn) + S(6), r.Y + (r.H - S(IconSize)) / 2, S(IconSize), S(IconSize));
        var detail = name with { Y = r.Y + S(29), H = S(18) };
        // On a job's own view, how it's going in place of the tick, as ticks are for what's chosen on the list.
        if (_job is not null)
            PaintMark(g, TickBox(r), row);
        else
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
        g.Text(row.Name, name, Fonts.Strong, row.Selected || _job is not null ? p.Text : p.Faint);
        var (said, saidColor) = Said(row);
        g.Text(said, detail, Fonts.Small, saidColor);

        var numbers = new Rect(0, r.Y, 0, r.H);
        if (columnNow > 0)
            g.Text(Was(row), numbers with { X = columnNow, W = S(ColumnNow) }, Fonts.Body, p.SubText, TextFlags.Line | TextFlags.Right);

        var (after, afterColor, saved, savedColor) = Result(row);
        bool shown = row.Selected || _job is not null;
        g.Text(after, numbers with { X = columnAfter, W = S(ColumnAfter) }, Fonts.Body, shown ? afterColor : p.Faint, TextFlags.Line | TextFlags.Right);
        if (columnSaved > 0)
        {
            // How much smaller as a bar too, as the Switch shows it, once it's known, and for the game being done, how far it's got.
            // On Done, what each game freed says it all, with no bar.
            double share = working ? StepFraction() ?? 0
                : OnDone ? 0
                : row.AfterBytes is { } afterBytes && row.InputBytes > 0 && row.State != RowState.Working && !(row.NotSure && row.State == RowState.Ready) ? 1 - (double)afterBytes / row.InputBytes : 0;
            if (share > 0)
            {
                g.Text(saved, numbers with { X = columnSaved, Y = r.Y + S(8), W = S(ColumnSaved), H = S(22) }, Fonts.Strong, shown ? savedColor : p.Faint, TextFlags.Line | TextFlags.Right);
                var bar = new Rect(columnSaved + S(ColumnSaved) - S(56), r.Y + S(33), S(56), S(4));
                // How far the game being done has got, or what a game saves, coloured from the left on a quiet track, as on the Switch: one
                // green, stronger the more it saves. What it saves glides to a new figure.
                double drawn = Math.Min(share, 1);
                if (working)
                    _barsShown.Remove(row.Key);
                else
                    drawn = BarShown(row.Key).Towards(drawn, Window);
                g.FillRound(bar, bar.H / 2, p.Border);
                g.FillRound(bar with { W = Math.Max(bar.H, (int)Math.Round(bar.W * drawn)) }, bar.H / 2,
                    !shown ? p.Faint : working ? savedColor : SavingColor.For(drawn));
            }
            else
            {
                _barsShown.Remove(row.Key);
                g.Text(saved, numbers with { X = columnSaved, W = S(ColumnSaved) }, Fonts.Strong, shown ? savedColor : p.Faint, TextFlags.Line | TextFlags.Right);
            }
        }

        // The info button is always there, so a game's Details are seen without looking for them.
        PaintRowButton(g, InfoButton(r), Glyph.Info, hovered && _hoverPart == Part.Info);
        if (hovered && !_busy)
        {
            if (row.OutputPath is not null)
                PaintRowButton(g, OutputButton(r), Glyph.FolderOpen, _hoverPart == Part.Output);
            if (CanRemove)
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
        g.Text(issue.Message, IssueText(r), Fonts.Small, p.Warn, TextFlags.Wrapped);
        if (hovered && !_busy)
            PaintRowButton(g, RemoveButton(r), Glyph.Close, _hoverPart == Part.Remove);
        g.Fill(new Rect(r.X + S(16), r.Bottom - 1, r.W - S(32), 1), p.Border);
    }

    private void PaintNote(Gfx g, int index, Rect r)
    {
        var p = Theme.P;
        string note = _notes[index - _rows.Length - _issues.Length];
        // Wrapped over as many lines as Measure made room for.
        g.Icon(Glyph.Info, new Rect(r.X + S(14), r.Y, S(20), S(NoteHeight)), 13, p.Faint);
        g.Text(note, NoteText(r) with { Y = r.Y + S(8), H = r.H - S(16) }, Fonts.Small, p.Faint, TextFlags.Wrapped);
    }

    private string ViewLabel(int index) => $"{(_views[index] switch { GamesShown.Done => "已完成", GamesShown.Everything => "全部", _ => "待处理" })} {_counts[index]}";

    /// <summary>For the test: what the list says along its top, and where each view is to click. A view's name says if it's shown.</summary>
    public IEnumerable<(string name, Rect bounds, string text)> TestParts()
    {
        if (Window is null || Bounds.W <= 0)
            yield break;
        if (_measuring)
            yield return ("measuring", Bounds, "");
        if (_job is { } job)
        {
            var (what, more) = job.Headline("to");
            yield return ("heading", Bounds, $"{what}; {more}");
        }
        else
        {
            if (IsEmpty)
                yield break;
            yield return ("heading", Bounds, string.Join(" | ", _views.Select((v, i) => (v == _view ? "*" : "") + ViewLabel(i))));
            for (int i = 0; i < _views.Length; i++)
                yield return ($"view_{_views[i].ToString().ToLowerInvariant()}", ViewArea(i), ViewLabel(i));
            yield return ("rows", Bounds, $"{_rows.Length}");
            if (Columns().now is > 0 and var now)
            {
                int width = Window.TextWidth("当前", Fonts.Small);
                yield return ("heading_now", new Rect(now + S(ColumnNow) - width, Bounds.Y, width, S(HeaderHeight)), "当前");
            }
        }
        // What the first games say under their names, and which have an info button, on the list or a job's own view.
        var (columnNow, columnAfter, _) = Columns();
        var area = ListArea;
        for (int i = 0; i < Math.Min(_rows.Length, 8); i++)
        {
            var row = new Rect(Bounds.X, area.Y + ItemTop(i) - _scroll.Offset, Bounds.W, ItemHeight(i));
            yield return ($"said_{i}", NameArea(row, columnNow, columnAfter) with { Y = row.Y + S(29), H = S(18) }, Said(_rows[i]).text);
            yield return ($"info_{i}", InfoButton(row), "");
            yield return ($"result_{i}", row, $"{Result(_rows[i]).after}|{Result(_rows[i]).saved}");
        }
    }

    private Rect NameArea(Rect r, int columnNow, int columnAfter)
    {
        int textRight = (columnNow > 0 ? columnNow : columnAfter) - S(8);
        int textLeft = r.X + S(TickColumn) + S(6) + S(IconSize) + S(12);
        return new Rect(textLeft, r.Y + S(8), textRight - textLeft, S(22));
    }

    /// <summary>What it's doing to how many games and what they come to, where the views are on the list, or once it's over, what it did.
    /// What they come to is left out rather than cut short where there's no room for all of it.</summary>
    private void PaintJobHeading(Gfx g, JobSnapshot job, int right)
    {
        var p = Theme.P;
        var (what, more) = job.Headline("\u2192");
        var color = job.Running ? p.Text : job.Outcome switch { UiLevel.Success => p.Good, UiLevel.Error => p.Bad, _ => p.Warn };
        int left = Bounds.X + S(TickColumn) + S(6);
        var area = new Rect(left, Bounds.Y, Math.Max(right - left, 0), S(HeaderHeight));
        int whatWidth = Math.Min(Window.TextWidth(what, Fonts.Strong) + S(2), area.W);
        g.Text(what, area with { W = whatWidth }, Fonts.Strong, color, TextFlags.Line);
        int moreWidth = Window.TextWidth(more, Fonts.Body) + S(2);
        if (more.Length > 0 && whatWidth + S(14) + moreWidth <= area.W)
            g.Text(more, area with { X = area.X + whatWidth + S(14), W = moreWidth }, Fonts.Body, p.SubText, TextFlags.Line);
    }

    /// <summary>How a game in a job is going: done, being done, waiting, couldn't be done, or left out.</summary>
    private void PaintMark(Gfx g, Rect box, TitleRow row)
    {
        var p = Theme.P;
        var round = box.Inset(S(1));
        switch (row.State)
        {
            case RowState.Done:
                g.FillRound(round, round.H / 2f, p.Good);
                g.Icon(Glyph.Check, round, 10, p.OnAccent);
                break;
            case RowState.Working:
                g.StrokeRound(round, round.H / 2f, p.Accent, S(2));
                g.FillRound(round.Inset(S(5)), round.Inset(S(5)).H / 2f, p.Accent);
                break;
            case RowState.Failed:
                g.FillRound(round, round.H / 2f, p.Bad);
                g.Icon(Glyph.Close, round, 9, p.OnAccent);
                break;
            default:
                g.StrokeRound(round, round.H / 2f, _job is { Running: false } && (row.BelowMinimum is not null || row.IsCompressed) ? p.Warn : p.StrongBorder);
                break;
        }
    }

    /// <summary>The line under a game's name: what it is, or on a job's own view how it's going, and what went wrong, in its colour.</summary>
    private (string text, Rgb color) Said(TitleRow row)
    {
        var p = Theme.P;
        // In a few plain words, as the failure itself, behind the info button, doesn't always manage.
        if (row.State == RowState.Failed && row.Failure is not null)
            return (Help.PlainReason(row.Failure, "详情按钮里有具体说明。"), p.Bad);
        if (_job is { } job)
        {
            switch (row.State)
            {
                case RowState.Working:
                    return (StepText(), p.Text);
                case RowState.Done:
                    return job.Running ? ("已完成", p.Good) : (Capitalized(row.Detail), p.SubText);
            }
            if (job.Running)
                return ("等待中", p.Faint);
            string? reason = row.BelowMinimum ?? (row.IsCompressed ? "已压缩" : null);
            return reason is null ? ("未完成", p.Faint) : ($"已跳过：{reason}", p.Warn);
        }
        // On Done, each game's own line, as that it saves nothing worth it goes without saying there, unless its developer compressed it.
        if (OnDone)
            return row.IsCompressed && row.Estimate?.ByMakers == true ? ("发行商已自行压缩", p.Good) : (Capitalized(row.Detail), p.SubText);
        return (Capitalized(row.DetailWithEstimate), row.BelowMinimum is not null ? p.Warn : p.SubText);
    }

    /// <summary>The step the game being done is on, in plain words, and how fast it's going.</summary>
    private string StepText()
    {
        if (_step is not { } step)
            return "正在开始";
        string text = ProgressReporter.Plain(step.Label);
        return step.ShowSizes && step.BytesPerSecond > 0 ? $"{text} \u00B7 {step.BytesPerSecond / 1048576:0} MB/s" : text;
    }

    private double? StepFraction() => _step is { Total: > 0 } step ? (double)step.Done / step.Total : null;

    /// <summary>Where the view shown is filled in, which slides to the next one shown.</summary>
    private readonly Glide _viewShown = new();
    /// <summary>Each game's bar as it's drawn, which glides to a new figure. One that goes comes back at its figure, as it was first shown.</summary>
    private readonly Dictionary<string, Glide> _barsShown = [];

    private Glide BarShown(string key) => _barsShown.TryGetValue(key, out var glide) ? glide : _barsShown[key] = new Glide();

    /// <summary>Where each view is in the control along the top, which is also what clicking it takes. Each is as wide as its words in
    /// bold, so nothing moves when another one is chosen.</summary>
    private Rect ViewArea(int index)
    {
        int x = Bounds.X + S(TickColumn) + S(6) + S(3);
        index = Math.Min(index, _views.Length - 1);
        for (int i = 0; i < index; i++)
            x += SegmentWidth(i);
        int height = S(28);
        return new Rect(x, Bounds.Y + (S(HeaderHeight) - height) / 2, SegmentWidth(index), height);
    }

    private int SegmentWidth(int index) => Window.TextWidth(ViewLabel(index), Fonts.Strong) + S(28);

    /// <summary>The whole control, around its views.</summary>
    private Rect ViewsControl()
    {
        var first = ViewArea(0);
        var last = ViewArea(_views.Length - 1);
        return new Rect(first.X - S(3), first.Y - S(3), last.Right - first.X + S(6), first.H + S(6));
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
        if (_send is { } send)
        {
            if (send.IsSettled(row.Files))
                return ("已发送", p.Good, "", p.Text);
            if (send.Of(row.Files) is { length: > 0 } sent)
                return ($"正在发送 {sent.read * 100 / sent.length}%", p.Accent, "", p.Text);
        }
        switch (row.State)
        {
            case RowState.Working when _job is not null:
                return ("", p.Accent, StepFraction() is { } fraction ? $"{fraction * 100:0}%" : "", p.Accent);
            case RowState.Working:
                return ("处理中…", p.Accent, "", p.Text);
            case RowState.Failed when _job?.Kind == SessionActivity.Verifying:
                return ("已损坏", p.Bad, "", p.Text);
            case RowState.Failed:
                return ("失败", p.Bad, "", p.Text);
            case RowState.Done when row.DoneAs == BatchMode.Extract:
                return ("已解包", p.Good, "", p.Text);
            case RowState.Done when row.DoneAs == BatchMode.Check:
                return ("完好", p.Good, "", p.Text);
            case RowState.Done when OnDone:
                return (ProgressReporter.Format(row.OutputBytes), p.Good, row.OutputBytes < row.InputBytes ? ProgressReporter.Format(row.InputBytes - row.OutputBytes) : "", p.Good);
            case RowState.Done:
                return (ProgressReporter.Format(row.OutputBytes), p.Good, Percent(row.InputBytes, row.OutputBytes), p.Good);
        }
        // On Done, what it takes and what compressing it freed, as on the Switch: in words where that isn't known, or where it was settled there
        // without being compressed.
        if (OnDone)
            return row.Freed > 0 ? (ProgressReporter.Format(row.InputBytes), p.Text, ProgressReporter.Format(row.Freed), p.Good)
                : (ProgressReporter.Format(row.InputBytes), p.Text, row.IsCompressed ? "未知" : "无", p.SubText);
        // Sure to save nothing: the size it stays at, and that in words, rather than 0% or a hair over its size. Beside what compressing it
        // freed, "Nothing" alone would read as if it had saved nothing.
        var nothing = (ProgressReporter.Format(row.InputBytes), p.SubText, row.IsCompressed ? "无可再省" : "无", p.SubText);
        // Not analyzed yet: said in words, as a figure is never left blank.
        if (row.Estimate is not { } estimate)
            return row.IsCompressed ? nothing : ("", p.Text, "尚未分析", p.SubText);
        // A first look can't vouch for it, so there's no figure to give until compressing has measured it.
        if (row.NotSure)
            return ("", p.Text, "不确定", p.SubText);
        if (!estimate.HasFigure(!Repacking))
            return nothing;
        // What it's sure to save, and what it comes to with that, so the two add up.
        string approximately = estimate.MarginBytes > 0 ? "~" : "";
        return (approximately + ProgressReporter.Format(estimate.MostBytes), p.Text, Percent(estimate.InputBytes, estimate.MostBytes), estimate.SavedBytes >= 0 ? p.Good : p.Warn);
    }

    /// <summary>The first figure: what it takes, or on Done what it took before compressing, when that's known.</summary>
    private string Was(TitleRow row) => !OnDone || row.State == RowState.Done ? ProgressReporter.Format(row.InputBytes)
        : row.Freed > 0 ? ProgressReporter.Format(row.InputBytes + row.Freed)
        : row.IsCompressed ? "未知" : ProgressReporter.Format(row.InputBytes);

    /// <summary>The list shows Done, where each game says what's become of it.</summary>
    private bool OnDone => _view == GamesShown.Done && _job is null;

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
        // Nor where the views along the top would run into its heading.
        if (now > 0 && Window is not null && _job is null && !IsEmpty && _views.Length > 0
            && ViewsControl().Right + S(12) > now + S(ColumnNow) - Window.TextWidth("当前", Fonts.Small))
            now = 0;
        return (now, after, saved);
    }

    private Rect RemoveButton(Rect row) => new(row.Right - S(16) - S(ButtonSize), row.Y + (row.H - S(ButtonSize)) / 2, S(ButtonSize), S(ButtonSize));

    /// <summary>How wide the line under a game's name is, which is the same on every row.</summary>
    private int DetailWidth()
    {
        var (columnNow, columnAfter, _) = Columns();
        return NameArea(new Rect(Bounds.X, 0, Bounds.W, S(RowHeight)), columnNow, columnAfter).W;
    }

    private Rect OutputButton(Rect row) => RemoveButton(row) with { X = row.Right - S(16) - S(ButtonSize) * 2 - S(2) };

    private Rect InfoButton(Rect row) => RemoveButton(row) with { X = row.Right - S(16) - S(ButtonSize) * 3 - S(4) };

    private (int index, Part part) PartAt(int x, int y)
    {
        var area = ListArea;
        if (_job is null && _rows.Length > 0 && y >= Bounds.Y && y < Bounds.Y + S(HeaderHeight) && x >= Bounds.X && x < Bounds.X + S(TickColumn) + S(12))
            return (-1, Part.TickAll);
        for (int i = 0; i < _views.Length && !IsEmpty && _job is null; i++)
        {
            if (ViewArea(i).Contains(x, y))
                return (i, Part.View);
        }
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
            if (i < _rows.Length + _issues.Length && CanRemove && RemoveButton(row).Contains(x, y))
                return (i, Part.Remove);
            if (i < _rows.Length && _rows[i].OutputPath is not null && OutputButton(row).Contains(x, y))
                return (i, Part.Output);
            if (i < _rows.Length && InfoButton(row).Contains(x, y))
                return (i, Part.Info);
            if (i < _rows.Length && _job is null && x < Bounds.X + S(TickColumn) + S(12))
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
        var (index, part) = PartAt(x, y);
        // Measuring goes on behind the list, so the views and a game's Details still open, as on the home screen and Android.
        // v19：后台读取期间点的 X 交给 Session 排队，等读完再删，别让它看起来毫无反应。
        if (_busy && part != Part.Remove && !(_measuring && (part == Part.Info || part == Part.View)))
            return;
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
            openDetails(_rows[index]);
        }
        else if (part == Part.View)
        {
            setView(_views[index]);
        }
    }

    public override void DoubleClick(int x, int y)
    {
        var (index, part) = PartAt(x, y);
        if (part == Part.Row && index < _rows.Length)
            openDetails(_rows[index]);
    }

    /// <summary>What can be done to the game, at the pointer, which it selects.</summary>
    public override void RightClick(int x, int y)
    {
        var (index, part) = PartAt(x, y);
        if (index < 0 || index >= _rows.Length || part is Part.View or Part.TickAll or Part.ScrollBar)
            return;
        _selected = index;
        gameMenu(_rows[index], new Rect(x, y, 0, 0));
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
            case Win32.VK_DELETE when _selected >= 0 && _selected < _rows.Length && !_busy && CanRemove:
                remove(_rows[_selected]);
                return true;
            case Win32.VK_SPACE when _selected >= 0 && _selected < _rows.Length && !_busy && _job is null:
                toggle(_rows[_selected]);
                return true;
            case Win32.VK_RETURN when _selected >= 0 && _selected < _rows.Length:
                openDetails(_rows[_selected]);
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
