namespace HacBrewPackLz;

/// <summary>
/// A game's Details, as on the Switch and in every version: the game on the left with the one figure that decides, its facts on the right in the
/// same four places, what's good to know below them, and along the bottom what can be done to it. The game is opened as the page opens, by a
/// reader that Look inside goes on with.
/// </summary>
internal sealed class DetailsPage : Page
{
    private readonly Session _session;
    private readonly string _key;
    private readonly InsideReader? _reader;
    /// <summary>Look inside is taking the reader over, so leaving doesn't close it.</summary>
    private bool _keepReader;
    private readonly Button _compress;
    private readonly Button _check;
    private readonly Button _extract;
    private readonly Button _look;
    private readonly Button _more;
    private int _scroll;
    private int _content;
    private int _room;
    private Picture? _icon;
    private byte[]? _iconFrom;
    private Rect _card;

    public DetailsPage(MainWindow window, Session session, string key, InsideReader? reader = null) : base(window)
    {
        _session = session;
        _key = key;
        var row = Row(session.Snapshot());
        // A game's folders are its files already, with nothing packed to open.
        _reader = reader ?? (row is { IsFolder: false } ? new InsideReader(row.Name, row.Files, session.Keys) : null);
        if (reader is null && _reader is not null)
            _reader.Changed += window.PostFromWorker;
        _look = Add(new Button("查看内部", ButtonStyle.Secondary, OpenInside));
        _extract = Add(new Button("解包", ButtonStyle.Ghost, () => Do(BatchMode.Extract)));
        _check = Add(new Button("检查损坏", ButtonStyle.Ghost, () => Do(BatchMode.Check)));
        _more = Add(new Button("", ButtonStyle.Ghost, ShowMore, Glyph.More));
        _compress = Add(new Button("压缩此游戏", ButtonStyle.Primary, () => Do(BatchMode.Convert)));
    }

    public override string Title => "详细信息";

    public override (string name, Widget widget)[] Named =>
        [("look_inside", _look), ("extract_this", _extract), ("check_this", _check), ("details_more", _more), ("compress_this", _compress)];

    /// <summary>For the tests: what it says, section by section, and its figure.</summary>
    public override IEnumerable<(string name, Rect bounds, string text)> TestParts
    {
        get
        {
            var s = _session.Snapshot();
            if (Row(s) is not { } row)
                yield break;
            var figure = GameDetails.Figure(row, Compressing);
            yield return ("details_figure", _card, $"{figure.Label} {figure.Big}");
            foreach (var section in GameDetails.Sections(row, _reader?.Inside, _reader?.Problem, Compressing))
                yield return ($"details_{section.Title.Replace(' ', '_')}", _card, string.Join("; ", section.Lines.Select(l => $"{l.Label}: {l.Value}")));
        }
    }

    private bool Compressing => _session.Settings.Get("--nocompress") != "1";

    private TitleRow? Row(SessionSnapshot s) => s.Rows.FirstOrDefault(r => r.Key == _key);

    private void Do(BatchMode mode)
    {
        bool started = mode switch
        {
            BatchMode.Extract => _session.Extract(_key),
            BatchMode.Check => _session.Check(_key),
            _ => _session.Compress(_key),
        };
        // The job shows on the list, as it does from there.
        if (started)
            Window.ClosePage();
    }

    private void OpenInside()
    {
        if (_reader?.Inside is null)
            return;
        _keepReader = true;
        Window.OpenPage(new InsidePage(Window, _session, _key, _reader));
    }

    private void ShowMore()
    {
        var items = new List<(string, char, Action)>();
        if (Row(_session.Snapshot()) is { OutputPath: { } output })
            items.Add(("在文件夹中显示", Glyph.FolderOpen, () => Explorer.Show(output)));
        items.Add(("从列表中移除", Glyph.Close, () =>
        {
            if (Row(_session.Snapshot()) is { } row)
            {
                _session.Remove(row);
                Window.ClosePage();
            }
        }));
        _ = new PopupList(Window, _more.Bounds with { W = S(220) }, items.ToArray());
    }

    public override bool OnKey(int key)
    {
        // L looks inside, as on the home screen.
        if (key == 'L' && _reader?.Inside is not null)
        {
            OpenInside();
            return true;
        }
        return false;
    }

    public override void OnWheel(int delta)
    {
        _scroll = Math.Clamp(_scroll - delta * S(48) / 120, 0, Math.Max(_content - _room, 0));
        Window.Invalidate();
    }

    /// <summary>Look inside goes on from here as soon as the game is open, as the game's menu asks.</summary>
    private bool _insideWhenOpen;

    public void OpenInsideWhenOpen()
    {
        _insideWhenOpen = true;
        OnWorker();
    }

    public override void OnWorker()
    {
        if (_insideWhenOpen && _reader is { Opening: false })
        {
            _insideWhenOpen = false;
            if (_reader.Inside is not null)
            {
                OpenInside();
                return;
            }
        }
        Window.Relayout();
    }

    public override void Leaving()
    {
        if (!_keepReader)
            _reader?.CloseInBackground();
        _icon?.Dispose();
        _icon = null;
    }

    public override void Layout(Rect card, Rect bar)
    {
        _card = card;
        var s = _session.Snapshot();
        var row = Row(s);
        bool free = !s.IsBusy || s.Activity == SessionActivity.Measuring;
        bool game = row is { IsFolder: false };
        _look.Visible = game;
        _look.Enabled = _reader?.Inside is not null;
        _extract.Visible = _check.Visible = game;
        _extract.Enabled = _check.Enabled = free;
        _compress.Text = Compressing ? "压缩此游戏" : "重打包此游戏";
        _compress.Visible = row is not null && !row.IsDone(BatchMode.Convert) && !(Compressing && row.IsCompressed);
        _compress.Enabled = free;
        _more.Visible = row is not null;
        PlaceBarButtons(bar, _compress, _check, _extract, _look, _more);
    }

    public override void Paint(Gfx g, Rect card, Rect bar)
    {
        var p = Theme.P;
        g.FillRound(card, S(10), p.Surface);
        var s = _session.Snapshot();
        if (Row(s) is not { } row)
        {
            g.Text("它已不在列表中。", card, Fonts.Body, p.SubText, TextFlags.Line | TextFlags.Center);
            return;
        }
        var inside = _reader?.Inside;

        // The game, on the left: its icon, its name, who made it, and at the bottom the figure that decides, with a bar for what it saves.
        var left = new Rect(card.X + S(16), card.Y + S(16), S(250), card.H - S(32));
        g.FillRound(left, S(10), Theme.IsDark ? p.Background : p.Hover);
        g.Clip(left);
        int iconSize = Math.Min(S(150), left.W - S(60));
        var iconArea = new Rect(left.X + (left.W - iconSize) / 2, left.Y + S(24), iconSize, iconSize);
        byte[]? iconData = inside?.Icon ?? row.Icon;
        if (!ReferenceEquals(iconData, _iconFrom) || (_icon is null && iconData is not null))
        {
            _icon?.Dispose();
            _icon = iconData is null ? null : Picture.Decode(iconData, iconSize);
            _iconFrom = iconData;
        }
        if (_icon is not null)
        {
            g.Image(_icon, iconArea);
        }
        else
        {
            g.FillRound(iconArea, S(14), p.Pressed);
            g.Icon(row.IsFolder ? Glyph.Folder : Glyph.Document, iconArea, 40, p.Faint);
        }
        int inner = left.W - S(36);
        int x = left.X + S(18);
        int y = iconArea.Bottom + S(18);
        int nameHeight = Math.Min(Window.TextHeight(row.Name, Fonts.Heading, inner), S(66));
        g.Text(row.Name, new Rect(x, y, inner, nameHeight), Fonts.Heading, p.Text, TextFlags.Wrapped);
        y += nameHeight + S(2);
        if (inside?.Publisher is { Length: > 0 } publisher)
        {
            g.Text(publisher, new Rect(x, y, inner, S(18)), Fonts.Small, p.SubText, TextFlags.Line);
            y += S(18);
        }
        var figure = GameDetails.Figure(row, Compressing);
        int figureHeight = S(18) + S(32) + (figure.Sub.Length > 0 ? S(18) : 0) + (figure.Share is not null ? S(14) : 0);
        int fy = Math.Max(left.Bottom - S(20) - figureHeight, y + S(14));
        g.Text(figure.Label, new Rect(x, fy, inner, S(18)), Fonts.Small, p.SubText, TextFlags.Line);
        fy += S(18);
        g.Text(figure.Big, new Rect(x, fy, inner, S(32)), Fonts.Title, p.Accent, TextFlags.Line);
        fy += S(32);
        if (figure.Sub.Length > 0)
        {
            g.Text(figure.Sub, new Rect(x, fy, inner, S(18)), Fonts.Small, p.SubText, TextFlags.Line);
            fy += S(18);
        }
        if (figure.Share is { } comesTo && comesTo < 1)
        {
            double saved = 1 - comesTo;
            var track = new Rect(x, fy + S(6), inner, S(6));
            g.FillRound(track, track.H / 2f, p.Border);
            g.FillRound(track with { W = Math.Max(track.H, (int)Math.Round(track.W * Math.Min(saved, 1))) }, track.H / 2f, SavingColor.For(saved));
        }
        g.ResetClip();

        // Its facts, on the right, in two columns where there's room for them, then what's good to know across both.
        var right = new Rect(left.Right + S(28), card.Y + S(18), card.Right - left.Right - S(28) - S(24), card.H - S(36));
        _room = right.H;
        var sections = GameDetails.Sections(row, inside, _reader?.Problem, Compressing);
        int labels = sections.SelectMany(x2 => x2.Lines).Max(l => Window.TextWidth(l.Label, Fonts.Body)) + S(20);
        bool two = right.W >= S(720);
        int column = two ? (right.W - S(36)) / 2 : right.W;
        g.Clip(right);
        int[] tops = [right.Y - _scroll, right.Y - _scroll];
        for (int i = 0; i < sections.Count; i++)
        {
            int at = two && i >= 2 ? 1 : 0;
            tops[at] = PaintSection(g, sections[i], right.X + at * (column + S(36)), tops[at], column, labels) + S(16);
        }
        int bottom = Math.Max(tops[0], tops[1]);
        var notes = GameDetails.Notes(row);
        if (notes.Count > 0)
        {
            g.Text("小提示", new Rect(right.X, bottom, right.W, S(22)), Fonts.Strong, p.Text, TextFlags.Line);
            bottom += S(26);
            foreach (string note in notes)
            {
                int h = Window.TextHeight(note, Fonts.Small, right.W);
                g.Text(note, new Rect(right.X, bottom, right.W, h), Fonts.Small, p.SubText, TextFlags.Wrapped);
                bottom += h + S(6);
            }
        }
        g.ResetClip();
        _content = bottom + _scroll - right.Y;
        if (_content > _room)
        {
            // How much more there is, as a thin bar along its right edge.
            int thumb = Math.Max(right.H * right.H / _content, S(24));
            int travel = right.H - thumb;
            g.FillRound(new Rect(card.Right - S(10), right.Y + travel * _scroll / Math.Max(_content - _room, 1), S(4), thumb), S(2), p.StrongBorder);
        }
    }

    /// <summary>One of the four places: its name, then each fact with its label in a column, its value, and a word more under it.</summary>
    private int PaintSection(Gfx g, DetailsSection section, int x, int y, int width, int labels)
    {
        var p = Theme.P;
        g.Text(section.Title, new Rect(x, y, width, S(22)), Fonts.Strong, p.Text, TextFlags.Line);
        y += S(28);
        int valueWidth = Math.Max(width - labels, S(80));
        foreach (var line in section.Lines)
        {
            var color = line.Bad ? p.Bad : line.Good ? p.Good : p.Text;
            g.Text(line.Label, new Rect(x, y, labels - S(8), S(20)), Fonts.Body, p.SubText, TextFlags.Line);
            int h = Math.Max(Window.TextHeight(line.Value, Fonts.Body, valueWidth), S(20));
            g.Text(line.Value, new Rect(x + labels, y, valueWidth, h), Fonts.Body, color, TextFlags.Wrapped);
            y += h;
            if (line.Note.Length > 0)
            {
                int noteHeight = Window.TextHeight(line.Note, Fonts.Small, valueWidth);
                g.Text(line.Note, new Rect(x + labels, y, valueWidth, noteHeight), Fonts.Small, p.Faint, TextFlags.Wrapped);
                y += noteHeight;
            }
            y += S(6);
        }
        return y;
    }
}

/// <summary>
/// Look inside, as on the Switch: the game's parts as folders on the left, what's under the pointer beside them, the way its kind is shown, and
/// any of it saved to Files from games. Find goes through every name in it. Back goes to the game's Details, which it came from.
/// </summary>
internal sealed class InsidePage : Page
{
    private readonly Session _session;
    private readonly string _key;
    private readonly InsideReader _reader;
    private bool _keepReader;
    private readonly InsideView _view;
    private readonly TextField _find;
    private readonly Button _save;
    private readonly Button _bytes;
    private readonly Button _shown;
    private Rect _barText;

    public InsidePage(MainWindow window, Session session, string key, InsideReader reader) : base(window)
    {
        _session = session;
        _key = key;
        _reader = reader;
        _view = Add(new InsideView(reader));
        _find = Add(new TextField("按名称查找文件或文件夹"));
        _find.Committed = () => _view.Find(_find.Value.Trim());
        _bytes = Add(new Button("按字节查看", ButtonStyle.Ghost, ToggleBytes));
        _shown = Add(new Button("在文件夹中显示", ButtonStyle.Ghost, () =>
        {
            if (_reader.SavedTo is { } saved)
                Explorer.Show(saved);
        }, Glyph.FolderOpen));
        _save = Add(new Button("保存", ButtonStyle.Primary, Save));
    }

    public override string Title => "查看内部";

    public override (string name, Widget widget)[] Named => [("inside_view", _view), ("find", _find), ("as_bytes", _bytes), ("show_saved", _shown), ("save", _save)];

    public override IEnumerable<(string name, Rect bounds, string text)> TestParts => [("inside", _view.Bounds, _view.Describe()), ("inside_status", _barText, Status().text)];

    /// <summary>Back to the game's Details, which it came from, still open. While something's being saved, Back stops that instead.</summary>
    public override bool GoBack()
    {
        if (_reader.Saving)
        {
            _reader.StopSaving();
            return true;
        }
        _keepReader = true;
        Window.OpenPage(new DetailsPage(Window, _session, _key, _reader));
        return true;
    }

    public override void Leaving()
    {
        if (!_keepReader)
            _reader.CloseInBackground();
        _view.Dispose();
    }

    private void ToggleBytes()
    {
        _view.AsBytes = !_view.AsBytes;
        _bytes.Text = _view.AsBytes ? "按原样查看" : "按字节查看";
        Window.Relayout();
    }

    private void Save()
    {
        if (_session.Snapshot().Rows.FirstOrDefault(r => r.Key == _key) is not { } row || _view.Selected < 0)
            return;
        _reader.Save(_view.Selected, GameInside.FilesFromGamesFor(row.Files[0], _session.Settings.Get("--outdir")));
        Window.Relayout();
    }

    public override bool OnKey(int key) => _view.KeyDown(key);

    public override void OnWheel(int delta) => _view.Wheel(delta);

    public override void OnWorker() => Window.Relayout();

    public override void Layout(Rect card, Rect bar)
    {
        _view.Bounds = card;
        _save.Enabled = !_reader.Saving && _view.Selected >= 0;
        _bytes.Visible = _reader.Inside is not null;
        _shown.Visible = _reader.SavedTo is not null && !_reader.Saving;
        int x = PlaceBarButtons(bar, _save, _bytes, _shown);
        int findWidth = Math.Min(S(260), Math.Max(x - bar.X - S(48), S(120)));
        _find.Bounds = new Rect(bar.X + S(16), bar.Y + (bar.H - S(32)) / 2, findWidth, S(32));
        _barText = new Rect(_find.Bounds.Right + S(16), bar.Y, Math.Max(x - _find.Bounds.Right - S(32), 0), bar.H);
    }

    /// <summary>Between Find and the buttons: how saving's going, where it went, or why it couldn't be.</summary>
    private (string text, Rgb color) Status()
    {
        var p = Theme.P;
        if (_reader.Saving)
            return (_reader.SavingFiles <= 1 ? $"正在保存  ·  已保存 {ProgressReporter.Format(_reader.SavedBytes)} / 共 {ProgressReporter.Format(_reader.SavingBytes)}"
                : $"正在保存第 {GameInside.WithCommas(Math.Min(_reader.SavedFiles + 1, _reader.SavingFiles))} / {GameInside.WithCommas(_reader.SavingFiles)} 个文件  ·  按 Esc 停止", p.Text);
        if (_reader.SaveProblem is { } problem)
            return (problem, p.Warn);
        if (_reader.SavedTo is { } saved)
            return ($"已保存到 {saved}", p.Good);
        return ("", p.Text);
    }

    public override void Paint(Gfx g, Rect card, Rect bar)
    {
        var (text, color) = Status();
        if (text.Length > 0 && _barText.W > 0)
            g.Text(text, _barText, Fonts.Small, color, TextFlags.Path);
    }
}

/// <summary>What Look inside shows: where it is along the top, what's in the folder on the left with a way up, and beside it what's under the
/// pointer, as a picture, text, bytes or what a folder is made of.</summary>
internal sealed class InsideView(InsideReader reader) : Widget, IDisposable
{
    private const int RowHeight = 30;
    private const int TopHeight = 44;

    private int _folder;
    private List<int>? _found;
    private string _finding = "";
    private int _selected;
    private readonly Stack<(int folder, int selected)> _above = new();
    private readonly ScrollMath _scroll = new();
    private int _previewScroll;
    private Picture? _picture;
    private (int index, int width, int height) _pictureFor = (-1, 0, 0);
    /// <summary>Where the pointer last was over it, which says whether the wheel goes through the text or the list.</summary>
    private int _mouseX, _mouseY;

    public bool AsBytes { get; set; }

    public override bool CanFocus => true;

    /// <summary>What's under the pointer, as the game has it, or -1 with nothing there.</summary>
    public int Selected => reader.Inside is { } inside && Shown(inside) is { Count: > 0 } shown && _selected < shown.Count ? shown[_selected] : -1;

    private List<int> Shown(GameInside inside) => _found ?? inside.Nodes[_folder].Children;

    /// <summary>A way up is the first thing in any folder but the top, as in a file manager.</summary>
    private bool HasUp => _found is not null || _folder != 0;

    private Rect ListArea => new(Bounds.X + S(12), Bounds.Y + S(TopHeight), Bounds.W * 9 / 20 - S(12), Bounds.H - S(TopHeight) - S(12));

    private Rect PreviewArea => new(ListArea.Right + S(20), Bounds.Y + S(TopHeight), Bounds.Right - ListArea.Right - S(36), Bounds.H - S(TopHeight) - S(12));

    public void Dispose()
    {
        _picture?.Dispose();
        _picture = null;
    }

    /// <summary>For the tests: where it is, what's under the pointer and what's shown for it.</summary>
    public string Describe()
    {
        if (reader.Inside is not { } inside)
            return reader.Problem ?? "opening";
        string where = _found is not null ? $"found {_finding}" : _folder == 0 ? "top" : inside.PathOf(_folder);
        int selected = Selected;
        string what = selected < 0 ? "" : inside.Nodes[selected].Name;
        string shown = reader.PreviewOf == selected && reader.PreviewAsBytes == AsBytes && reader.Preview is { } preview ? string.Join(" · ", preview.Chips) : "reading";
        return $"{where} | {what} | {shown}";
    }

    public void Find(string text)
    {
        if (reader.Inside is not { } inside || text.Length == 0)
            return;
        var found = inside.Search(text);
        if (found.Count == 0)
        {
            _found = [];
            _finding = text;
        }
        else
        {
            if (_found is null)
                _above.Push((_folder, _selected));
            _found = found;
            _finding = text;
        }
        Moved(0);
    }

    private void Moved(int to)
    {
        _selected = to;
        _previewScroll = 0;
        if (reader.Inside is { } inside)
        {
            int count = Shown(inside).Count + (HasUp ? 1 : 0);
            int row = _selected + (HasUp ? 1 : 0);
            _scroll.Measure(count * S(RowHeight), Math.Max(ListArea.H, 1));
            _scroll.ScrollInto(row * S(RowHeight), S(RowHeight));
        }
        Window.Invalidate();
    }

    private void Open(int index)
    {
        if (reader.Inside is not { } inside || !inside.Nodes[index].Folder)
            return;
        _above.Push((_found is null ? _folder : -1, _selected));
        _folder = index;
        _found = null;
        Moved(0);
    }

    private void Up()
    {
        if (reader.Inside is not { } inside)
            return;
        if (_above.Count > 0)
        {
            var (folder, selected) = _above.Pop();
            if (folder < 0)
                _found = inside.Search(_finding);
            else
            {
                _found = null;
                _folder = folder;
            }
            Moved(selected);
            return;
        }
        if (_found is not null)
        {
            _found = null;
            Moved(0);
            return;
        }
        int child = _folder;
        _folder = inside.Nodes[_folder].Parent;
        Moved(Math.Max(inside.Nodes[_folder].Children.IndexOf(child), 0));
    }

    public override bool KeyDown(int key)
    {
        if (reader.Inside is not { } inside)
            return false;
        int count = Shown(inside).Count;
        switch (key)
        {
            case Win32.VK_UP:
                Moved(Math.Max(_selected - 1, 0));
                return true;
            case Win32.VK_DOWN:
                Moved(Math.Clamp(_selected + 1, 0, Math.Max(count - 1, 0)));
                return true;
            case Win32.VK_HOME:
                Moved(0);
                return true;
            case Win32.VK_END:
                Moved(Math.Max(count - 1, 0));
                return true;
            case 0x21:
                Moved(Math.Max(_selected - 10, 0));
                return true;
            case 0x22:
                Moved(Math.Clamp(_selected + 10, 0, Math.Max(count - 1, 0)));
                return true;
            case Win32.VK_RETURN or Win32.VK_RIGHT when Selected >= 0:
                Open(Selected);
                return true;
            case 0x08 or Win32.VK_LEFT when HasUp:
                Up();
                return true;
        }
        return false;
    }

    /// <summary>Over the preview, a long text goes up and down; anywhere else, the list.</summary>
    public override bool Wheel(int delta)
    {
        if (PreviewArea.Contains(_mouseX, _mouseY))
            _previewScroll = Math.Max(_previewScroll - delta * 3 / 120, 0);
        else
            _scroll.ScrollBy(-delta * S(RowHeight) * 3 / 120);
        Window.Invalidate();
        return true;
    }

    private int RowAt(int y) => (y - ListArea.Y + _scroll.Offset) / S(RowHeight);

    public override void MouseMove(int x, int y) => (_mouseX, _mouseY) = (x, y);

    public override void MouseDown(int x, int y)
    {
        if (reader.Inside is not { } inside || !ListArea.Contains(x, y))
            return;
        int row = RowAt(y);
        if (HasUp && row == 0)
        {
            Up();
            return;
        }
        int index = row - (HasUp ? 1 : 0);
        if (index >= 0 && index < Shown(inside).Count)
            Moved(index);
    }

    public override void DoubleClick(int x, int y)
    {
        if (ListArea.Contains(x, y) && Selected >= 0 && RowAt(y) - (HasUp ? 1 : 0) == _selected)
            Open(Selected);
    }

    public override void Paint(Gfx g)
    {
        var p = Theme.P;
        g.FillRound(Bounds, S(10), p.Surface);
        if (reader.Inside is not { } inside)
        {
            string said = reader.Problem is { } problem ? $"无法打开：{problem}" : "正在打开…";
            g.Text(said, Bounds.Inset(S(24)), Fonts.Body, reader.Problem is null ? p.SubText : p.Bad, TextFlags.Wrapped | TextFlags.Center);
            return;
        }
        // Where it is, along the top.
        string where = _found is not null ? (_found.Count == 0 ? $"其中没有名为「{_finding}」的内容" : $"找到 {GameInside.WithCommas(_found.Count)} 个名为「{_finding}」的内容")
            : _folder == 0 ? $"{inside.Name}  ·  {GameInside.WithCommas(inside.Files)} 个文件，{ProgressReporter.Format(inside.Bytes)}"
            : $"{inside.Name}  ›  {inside.PathOf(_folder).Replace("/", "  ›  ")}";
        g.Text(where, new Rect(Bounds.X + S(20), Bounds.Y + S(8), Bounds.W - S(40), S(28)), Fonts.Strong, p.Text, TextFlags.Path);
        g.Fill(new Rect(Bounds.X + S(12), Bounds.Y + S(TopHeight) - S(4), Bounds.W - S(24), 1), p.Border);

        // What's in the folder, a way up first.
        var shown = Shown(inside);
        var list = ListArea;
        int count = shown.Count + (HasUp ? 1 : 0);
        _scroll.Measure(count * S(RowHeight), Math.Max(list.H, 1));
        _selected = Math.Clamp(_selected, 0, Math.Max(shown.Count - 1, 0));
        g.Clip(list);
        int first = _scroll.Offset / S(RowHeight);
        for (int row = first; row < Math.Min(count, first + list.H / S(RowHeight) + 2); row++)
        {
            var r = new Rect(list.X, list.Y + row * S(RowHeight) - _scroll.Offset, list.W, S(RowHeight));
            if (HasUp && row == 0)
            {
                g.Icon(Glyph.Back, new Rect(r.X + S(8), r.Y, S(18), r.H), 12, p.SubText);
                g.Text("返回上一级", new Rect(r.X + S(34), r.Y, r.W - S(40), r.H), Fonts.Body, p.SubText, TextFlags.Line);
                continue;
            }
            int index = row - (HasUp ? 1 : 0);
            var node = inside.Nodes[shown[index]];
            bool chosen = index == _selected;
            if (chosen)
                g.FillRound(r, S(6), IsFocused ? p.Pressed : p.Hover);
            g.Icon(node.Folder ? Glyph.Folder : Glyph.Document, new Rect(r.X + S(8), r.Y, S(18), r.H), 12, node.Folder ? p.Accent : p.SubText);
            string size = node.Folder ? GameInside.ItemsWord(node.Items) : ProgressReporter.Format(node.Size);
            int sizeWidth = S(84);
            int kindWidth = node.Folder ? 0 : S(64);
            // What Find found says where it is, as it can be anywhere.
            string name = _found is not null && node.Parent > 0 ? $"{node.Name}   {inside.PathOf(node.Parent)}" : node.Name;
            g.Text(name, new Rect(r.X + S(34), r.Y, r.W - S(34) - sizeWidth - kindWidth - S(12), r.H), chosen ? Fonts.Strong : Fonts.Body, p.Text, TextFlags.Line);
            g.Text(size, new Rect(r.Right - sizeWidth - kindWidth - S(8), r.Y, sizeWidth, r.H), Fonts.Small, p.SubText, TextFlags.Line | TextFlags.Right);
            if (kindWidth > 0)
                g.Text(node.Kind.ToLowerInvariant(), new Rect(r.Right - kindWidth, r.Y, kindWidth - S(8), r.H), Fonts.Small, p.Faint, TextFlags.Line | TextFlags.Right);
        }
        g.ResetClip();
        if (_scroll.Max > 0)
            _scroll.Paint(g, new Rect(list.Right - S(4), list.Y + S(4), S(4), list.H - S(8)), false);

        g.Fill(new Rect(list.Right + S(10), list.Y, 1, list.H), p.Border);
        PaintPreview(g, inside);
        if (IsFocused && Window.KeyboardCues)
            g.StrokeRound(Bounds, S(10), p.Accent, S(2));
    }

    /// <summary>What's under the pointer, the way its kind is shown: a picture as big as it is or fitted, text with its lines numbered, bytes,
    /// or what a folder's made of. What's still being read says so.</summary>
    private void PaintPreview(Gfx g, GameInside inside)
    {
        var p = Theme.P;
        var area = PreviewArea;
        int selected = Selected;
        if (selected < 0)
            return;
        reader.Want(selected, AsBytes);
        if (reader.PreviewOf != selected || reader.PreviewAsBytes != AsBytes || reader.Preview is not { } preview)
        {
            g.Text("正在读取…", area with { H = S(22) }, Fonts.Body, p.SubText, TextFlags.Line);
            return;
        }
        g.Text(string.Join("  ·  ", preview.Chips.Select(InsideZh.Chip)), area with { H = S(22) }, Fonts.Small, p.SubText, TextFlags.Line);
        var body = new Rect(area.X, area.Y + S(30), area.W, area.H - S(30));
        if (preview.Picture is { } bytes)
        {
            int roomHeight = preview.Kind == "Folder" ? body.H / 2 : body.H;
            if (_pictureFor != (selected, body.W, roomHeight))
            {
                _picture?.Dispose();
                _picture = Picture.DecodeFitted(bytes, body.W, roomHeight, out _, out _);
                _pictureFor = (selected, body.W, roomHeight);
            }
            if (_picture is not null)
            {
                var spot = new Rect(body.X + (body.W - _picture.Width) / 2, body.Y + (roomHeight - _picture.Height) / 2, _picture.Width, _picture.Height);
                g.Image(_picture, spot);
            }
            if (preview.Kind != "Folder")
                return;
            body = body with { Y = body.Y + roomHeight + S(12), H = body.H - roomHeight - S(12) };
        }
        if (preview.Kind == "Folder")
        {
            int y = body.Y;
            foreach (var (kind, files, total) in preview.Breakdown)
            {
                if (y + S(22) > body.Bottom)
                    break;
                g.Text(InsideZh.Chip(kind), new Rect(body.X, y, S(110), S(22)), Fonts.Body, p.Text, TextFlags.Line);
                g.Text(files == 1 ? "1 个文件" : $"{GameInside.WithCommas(files)} 个文件", new Rect(body.X + S(110), y, S(110), S(22)), Fonts.Body, p.SubText, TextFlags.Line | TextFlags.Right);
                g.Text(ProgressReporter.Format(total), new Rect(body.X + S(230), y, S(100), S(22)), Fonts.Body, p.SubText, TextFlags.Line | TextFlags.Right);
                y += S(24);
            }
            return;
        }
        // Text and bytes, a line at a time from where it's been scrolled to.
        List<string> lines;
        if (preview.Kind == "Text")
        {
            int number = preview.Lines.Count.ToString().Length;
            lines = preview.Lines.Select((line, i) => (i + 1).ToString().PadLeft(number) + "  " + line).ToList();
        }
        else if (preview.Raw is { } raw)
        {
            // As many bytes a line as fit beside the list: 16, 8 or 4.
            int columns = body.W / Math.Max(Window.TextWidth("0000000000", Fonts.Mono) / 10, 1);
            int perLine = columns >= 10 + 16 * 4 + 1 ? 16 : columns >= 10 + 8 * 4 + 1 ? 8 : 4;
            lines = GameInside.ByteLines(raw, perLine, raw.Length).ToList();
        }
        else
        {
            return;
        }
        int lineHeight = S(18);
        _previewScroll = Math.Clamp(_previewScroll, 0, Math.Max(lines.Count - body.H / lineHeight, 0));
        g.Clip(body);
        for (int i = _previewScroll, y = body.Y; i < lines.Count && y < body.Bottom; i++, y += lineHeight)
            g.Text(lines[i], new Rect(body.X, y, body.W, lineHeight), Fonts.Mono, p.Text, TextFlags.SingleLine | TextFlags.NoPrefix);
        g.ResetClip();
    }
}


/// <summary>
/// v18：GameInside 的 Kind 值同时被 GamePages 用 == 判定（preview.Kind == "Folder" / "Text"），
/// 所以数据源保持英文，只在显示层把 chips / breakdown 映射成中文。
/// </summary>
internal static class InsideZh
{
    private static readonly Dictionary<string, string> Kinds = new(StringComparer.Ordinal)
    {
        ["Folder"] = "文件夹", ["Text"] = "文本", ["Picture"] = "图片", ["Texture"] = "纹理",
        ["Sound"] = "音频", ["Video"] = "视频", ["Archive"] = "压缩包", ["Data"] = "数据",
        ["Empty"] = "空", ["Unknown"] = "未知", ["As bytes"] = "按字节", ["As it is"] = "按原样",
        ["1 item"] = "1 项", ["1 file"] = "1 个文件", ["1 line"] = "1 行",
    };

    public static string Chip(string chip)
    {
        if (Kinds.TryGetValue(chip, out string? zh))
            return zh;
        var m = System.Text.RegularExpressions.Regex.Match(chip, @"^([\d,]+) items$");
        if (m.Success)
            return m.Groups[1].Value + " 项";
        m = System.Text.RegularExpressions.Regex.Match(chip, @"^([\d,]+) files$");
        if (m.Success)
            return m.Groups[1].Value + " 个文件";
        m = System.Text.RegularExpressions.Regex.Match(chip, @"^([\d,]+) (?:line|lines)(.*)$");
        if (m.Success)
        {
            string rest = m.Groups[2].Value;
            if (rest.StartsWith(" in its first ", StringComparison.Ordinal))
                return m.Groups[1].Value + " 行（前 " + rest[14..] + "）";
            return m.Groups[1].Value + " 行";
        }
        if (chip.StartsWith("Showing ", StringComparison.Ordinal))
            return "预览：" + chip[8..];
        if (chip.StartsWith("Its first ", StringComparison.Ordinal))
            return "前 " + chip[10..].Replace(" as bytes", " 的字节内容");
        return chip;
    }
}
