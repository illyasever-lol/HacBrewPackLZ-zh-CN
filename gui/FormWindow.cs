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

/// <summary>Values that only matter while the window is open.</summary>
internal sealed class LocalStore : IFormStore
{
    private readonly Dictionary<string, string> _values = [];

    public string Get(string flag) => _values.GetValueOrDefault(flag, "");

    public string? Set(OptionSpec option, string value)
    {
        _values[option.Flag] = value;
        return null;
    }
}

/// <param name="运行">Does the work and closes the window, or returns what's still missing and leaves it open.</param>
internal sealed record FormAction(string Text, ButtonStyle Style, Func<string?> Run);

/// <summary>A window of options with pages down the side, made from the same list the command line help comes from.</summary>
internal sealed class FormWindow : AppWindow
{
    private const int SidebarWidth = 190;
    private const int MinimumRowHeight = 58;
    private const int FooterHeight = 64;
    private const int HeaderHeight = 58;

    private sealed record Row(OptionSpec Option, Widget[] Controls);

    private readonly OptionGroup[] _groups;
    private readonly IFormStore _store;
    private readonly Action<OptionSpec>? _run;
    private readonly Func<OptionSpec, string?>? _outcome;
    private readonly List<Button> _pages = [];
    private readonly List<Row> _rows = [];
    private readonly List<Button> _actions = [];
    private int[] _rowTops = [];
    private readonly Button _reset;
    private readonly ScrollMath _scroll = new();
    private int _page;
    private string? _error;
    private string? _errorFlag;

    /// <param name="run">Does what an action among the options is for.</param>
    /// <param name="outcome">How an action went, shown beside its button.</param>
    public FormWindow(AppWindow owner, string title, OptionGroup[] groups, IFormStore store, FormAction[] actions,
        Action<OptionSpec>? run = null, Func<OptionSpec, string?>? outcome = null)
    {
        _groups = groups;
        _store = store;
        _run = run;
        _outcome = outcome;
        CreateTopLevel(title, groups.Length > 1 ? 780 : 640, 560, owner);

        if (groups.Length > 1)
        {
            for (int i = 0; i < groups.Length; i++)
            {
                int page = i;
                _pages.Add(Add(new Button(groups[i].Title, ButtonStyle.Ghost, () => ShowPage(page)) { AlignLeft = true }));
            }
        }

        _reset = Add(new Button("重置本页", ButtonStyle.Ghost, ResetPage));
        _reset.Visible = store is SettingsStore;
        foreach (var action in actions)
        {
            _actions.Add(Add(new Button(action.Text, action.Style, () =>
            {
                CommitFocused();
                if (_error is not null)
                    return;
                _error = action.Run();
                if (_error is null)
                    Close();
            })));
        }

        ShowPage(0);
    }

    protected override (int width, int height) MinimumSize => (560, 380);

    protected override bool OnKey(int key)
    {
        if (key == Win32.VK_ESCAPE)
        {
            Close();
            return true;
        }
        return false;
    }

    protected override void OnWheel(int delta) => ScrollBy(-delta * S(MinimumRowHeight) / 120);

    private void CommitFocused()
    {
        if (Focused is TextField field && _rows.FirstOrDefault(r => r.Controls.Contains(field)) is { } row)
            Commit(row.Option, field.Value.Trim(), field);
    }

    private void ShowPage(int page)
    {
        foreach (var row in _rows)
        {
            foreach (var control in row.Controls)
                Remove(control);
        }
        _rows.Clear();
        _page = page;
        _error = null;
        _errorFlag = null;
        foreach (var option in _groups[page].Options)
            _rows.Add(new Row(option, CreateControls(option)));
        _scroll.Measure(0, 0);
        _scroll.ScrollBy(int.MinValue / 2);
        Relayout();
    }

    private Widget[] CreateControls(OptionSpec option)
    {
        string value = _store.Get(option.Flag);
        switch (option.Kind)
        {
            case OptionKind.Toggle:
                var toggle = new ToggleSwitch(on => Commit(option, option.ToggleValue(on != option.Inverted), null)) { On = option.ToggleOn(value) != option.Inverted };
                return [Add(toggle)];
            case OptionKind.Slider:
                var slider = new Slider(option.Min, option.Max, chosen => Commit(option, chosen.ToString() == option.Default ? "" : chosen.ToString(), null))
                {
                    Value = int.TryParse(value.Length > 0 ? value : option.Default, out int number) ? number : option.Min,
                };
                return [Add(slider)];
            case OptionKind.Choice:
                if (!option.Editable)
                {
                    var dropdown = new Dropdown(option.Choices, chosen => Commit(option, chosen == option.Default ? "" : chosen, null))
                    {
                        Value = value.Length > 0 ? value : option.Default,
                    };
                    return [Add(dropdown)];
                }
                else
                {
                    // 预置项之外还能自己填版本号；输入框的空提示就是填写范例。
                    var custom = Add(new TextField(option.Placeholder));
                    custom.Value = value;
                    var preset = new Dropdown(option.Choices, chosen =>
                    {
                        string chosenValue = chosen == option.Default ? "" : chosen;
                        custom.Value = chosenValue;
                        Commit(option, chosenValue, custom);
                    })
                    {
                        Value = Array.IndexOf(option.Choices, value) >= 0 ? value : option.Default,
                    };
                    Add(preset);
                    AttachCommit(custom, option);
                    return [custom, preset];
                }
            case OptionKind.Action:
                return [Add(new Button(option.Verb, ButtonStyle.Secondary, () =>
                {
                    _run?.Invoke(option);
                    Invalidate();
                }))];
            case OptionKind.File or OptionKind.Folder:
                var path = Add(new TextField(option.Placeholder));
                path.Value = value;
                var browse = Add(new Button("浏览", ButtonStyle.Secondary, () => Browse(option, path)));
                AttachCommit(path, option);
                return [path, browse];
            default:
                var text = Add(new TextField(option.Placeholder));
                text.Value = value;
                AttachCommit(text, option);
                return [text];
        }
    }

    private void AttachCommit(TextField field, OptionSpec option)
    {
        field.Committed = () => Commit(option, field.Value.Trim(), field);
    }

    private void Commit(OptionSpec option, string value, TextField? field)
    {
        string? problem = _store.Set(option, value);
        if (field is not null)
            field.HasError = problem is not null;
        if (problem is not null)
        {
            _error = problem;
            _errorFlag = option.Flag;
        }
        else if (_errorFlag == option.Flag)
        {
            _error = null;
            _errorFlag = null;
        }
        Invalidate();
    }

    private void Browse(OptionSpec option, TextField field)
    {
        string? chosen;
        if (option.Kind == OptionKind.Folder)
        {
            chosen = FileDialogs.OpenFolders(Handle, $"Choose: {option.Label}", multiple: false).FirstOrDefault();
        }
        else if (option.Flag == "--output")
        {
            chosen = FileDialogs.SaveFile(Handle, "将 NSP 另存为", "NSP 文件", "*.nsp", "nsp", field.Value);
        }
        else
        {
            bool keys = option.Flag == "--keyset";
            chosen = FileDialogs.OpenFiles(Handle, $"Choose: {option.Label}", keys ? "密钥" : "NSP 文件", keys ? "*.keys;*.txt;*.dat;*.ini" : "*.nsp", multiple: false).FirstOrDefault();
        }
        if (chosen is null)
            return;
        field.Value = chosen;
        Commit(option, chosen, field);
    }

    private void ResetPage()
    {
        foreach (var option in _groups[_page].Options.Where(o => o.Kind != OptionKind.Action))
            _store.Set(option, "");
        ShowPage(_page);
    }

    private int ContentLeft => _groups.Length > 1 ? S(SidebarWidth) : S(8);

    private Rect ContentArea => new(ContentLeft, S(HeaderHeight), ClientWidth - ContentLeft, ClientHeight - S(HeaderHeight) - S(FooterHeight));

    protected override void Layout(int width, int height)
    {
        if (_reset is null)
            return;

        for (int i = 0; i < _pages.Count; i++)
        {
            _pages[i].Bounds = new Rect(S(12), S(HeaderHeight) + i * S(38), S(SidebarWidth) - S(24), S(34));
        }

        // Footer
        int x = width - S(20);
        int y = height - S(FooterHeight) + (S(FooterHeight) - S(36)) / 2;
        for (int i = _actions.Count - 1; i >= 0; i--)
        {
            int w = Math.Max(_actions[i].PreferredWidth(), S(88));
            x -= w;
            _actions[i].Bounds = new Rect(x, y, w, S(36));
            x -= S(8);
        }
        _reset.Bounds = new Rect(ContentLeft + S(16), y, _reset.PreferredWidth(), S(36));

        // Rows, each as tall as its help text needs.
        var area = ContentArea;
        int rowsRight = area.Right - S(28);
        int rowsLeft = area.X + S(16);
        int controlX = rowsLeft + (rowsRight - rowsLeft) * 46 / 100;
        _rowTops = new int[_rows.Count + 1];
        for (int i = 0; i < _rows.Count; i++)
        {
            int help = TextHeight(_rows[i].Option.Help, Fonts.Small, controlX - rowsLeft - S(16));
            _rowTops[i + 1] = _rowTops[i] + Math.Max(S(MinimumRowHeight), S(10) + S(20) + help + S(10));
        }
        _scroll.Measure(_rowTops[^1], area.H);

        for (int i = 0; i < _rows.Count; i++)
        {
            int rowY = area.Y + _rowTops[i] - _scroll.Offset;
            int controlHeight = S(32);
            int controlY = rowY + (_rowTops[i + 1] - _rowTops[i] - controlHeight) / 2;
            var controls = _rows[i].Controls;
            foreach (var control in controls)
                control.ClipTo = area;
            if (_rows[i].Option.Kind == OptionKind.Action && controls[0] is Button action)
            {
                action.Bounds = new Rect(controlX, controlY, action.PreferredWidth(), controlHeight);
            }
            else if (controls.Length == 2)
            {
                // 第二个控件可能是「浏览」按钮，也可能是版本号预置下拉。
                int secondWidth = controls[1] is Button browse ? browse.PreferredWidth() : S(210);
                controls[1].Bounds = new Rect(rowsRight - secondWidth, controlY, secondWidth, controlHeight);
                controls[0].Bounds = new Rect(controlX, controlY, rowsRight - secondWidth - S(8) - controlX, controlHeight);
            }
            else
            {
                controls[0].Bounds = new Rect(controlX, controlY, rowsRight - controlX, controlHeight);
            }
        }
    }

    protected override void PaintWindow(Gfx g)
    {
        var p = Theme.P;
        var area = ContentArea;

        if (_groups.Length > 1)
        {
            g.Fill(new Rect(S(SidebarWidth) - 1, 0, 1, ClientHeight), p.Border);
            for (int i = 0; i < _pages.Count; i++)
            {
                _pages[i].Style = ButtonStyle.Ghost;
                if (i == _page)
                {
                    var b = _pages[i].Bounds;
                    g.FillRound(b, S(6), p.Hover);
                    g.FillRound(new Rect(b.X, b.Y + S(8), S(3), b.H - S(16)), S(2), p.Accent);
                }
            }
        }

        g.Text(_groups[_page].Title, new Rect(area.X + S(16), S(14), area.W - S(32), S(32)), Fonts.Heading, p.Text);
        g.Fill(new Rect(area.X, S(HeaderHeight) - 1, area.W, 1), p.Border);

        g.Clip(area);
        int rowsLeft = area.X + S(16);
        int rowsRight = area.Right - S(28);
        int controlX = rowsLeft + (rowsRight - rowsLeft) * 46 / 100;
        for (int i = 0; i < _rows.Count && i + 1 < _rowTops.Length; i++)
        {
            int rowY = area.Y + _rowTops[i] - _scroll.Offset;
            var option = _rows[i].Option;
            int textWidth = controlX - rowsLeft - S(16);
            g.Text(option.Label, new Rect(rowsLeft, rowY + S(9), textWidth, S(20)), Fonts.Body, p.Text);
            g.Text(option.Help, new Rect(rowsLeft, rowY + S(29), textWidth, _rowTops[i + 1] - _rowTops[i] - S(29)), Fonts.Small, p.SubText, TextFlags.WordBreak | TextFlags.EditControl | TextFlags.NoPrefix);
            if (option.Kind == OptionKind.Action && _outcome?.Invoke(option) is { } outcome)
            {
                var button = _rows[i].Controls[0].Bounds;
                g.Text(outcome, new Rect(button.Right + S(12), button.Y, rowsRight - button.Right - S(12), button.H), Fonts.Small, p.SubText, TextFlags.Line);
            }
        }
        g.ResetClip();
        _scroll.Paint(g, new Rect(area.Right - S(8), area.Y + S(4), S(4), area.H - S(8)), false);

        g.Fill(new Rect(area.X, area.Bottom, area.W, 1), p.Border);
        if (_error is not null)
        {
            int left = _reset.Visible ? _reset.Bounds.Right + S(8) : area.X + S(16);
            int right = _actions.Count > 0 ? _actions[0].Bounds.X - S(8) : area.Right - S(16);
            g.Text(_error, new Rect(left, area.Bottom + S(6), right - left, S(FooterHeight) - S(12)), Fonts.Small, p.Bad, TextFlags.Wrapped | TextFlags.VCenter);
        }
    }

    private void ScrollBy(int delta)
    {
        _scroll.ScrollBy(delta);
        Relayout();
    }
}

/// <summary>Looks through a location for game files, to pick which ones to add.</summary>
internal sealed class SearchWindow : AppWindow
{
    private static string? s_lastLocation;

    private readonly Action<List<string>> _add;
    private readonly TextField _location;
    private readonly TextField _filter;
    private readonly Button _browse;
    private readonly Button _go;
    private readonly FoundList _list;
    private readonly Button _selectAll;
    private readonly Button _addButton;
    private readonly Button _cancel;
    private readonly object _gate = new();
    private readonly List<FoundFile> _found = [];
    private CancellationTokenSource? _stop;
    private bool _searching;
    private int _refreshPending;
    private string _status = "";

    public SearchWindow(AppWindow owner, Action<List<string>> add)
    {
        _add = add;
        CreateTopLevel("搜索位置", 760, 560, owner);

        _location = Add(new TextField("要搜索的文件夹或驱动器"));
        _location.Value = s_lastLocation ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads");
        _browse = Add(new Button("浏览", ButtonStyle.Secondary, Browse));
        _go = Add(new Button("搜索", ButtonStyle.Secondary, () => StartSearch()));
        _filter = Add(new TextField("按名称筛选"));
        _filter.Changed = ApplyFilter;
        _list = Add(new FoundList(UpdateAddButton));
        _selectAll = Add(new Button("全选", ButtonStyle.Ghost, ToggleAll));
        _cancel = Add(new Button("取消", ButtonStyle.Secondary, Close));
        _addButton = Add(new Button("添加", ButtonStyle.Primary, AddChosen));

        StartSearch();
        Relayout();
    }

    protected override (int width, int height) MinimumSize => (560, 380);

    protected override bool OnKey(int key)
    {
        if (key == Win32.VK_ESCAPE)
        {
            Close();
            return true;
        }
        if (key == Win32.VK_RETURN && Focused == _location)
        {
            StartSearch();
            return true;
        }
        return false;
    }

    protected override void OnClosed() => _stop?.Cancel();

    protected override void OnMessageFromWorker()
    {
        Interlocked.Exchange(ref _refreshPending, 0);
        ApplyFilter();
    }

    protected override void Layout(int width, int height)
    {
        if (_addButton is null)
            return;
        int pad = S(20);
        int y = S(16);
        int browseWidth = _browse.PreferredWidth();
        int goWidth = _go.PreferredWidth();
        _go.Bounds = new Rect(width - pad - goWidth, y, goWidth, S(34));
        _browse.Bounds = new Rect(_go.Bounds.X - S(8) - browseWidth, y, browseWidth, S(34));
        _location.Bounds = new Rect(pad, y, _browse.Bounds.X - S(8) - pad, S(34));
        y += S(42);
        _filter.Bounds = new Rect(pad, y, width - pad * 2, S(34));
        y += S(44);

        int footer = S(64);
        _list.Bounds = new Rect(pad, y, width - pad * 2, height - y - footer);

        int buttonY = height - footer + (footer - S(36)) / 2;
        int addWidth = Math.Max(_addButton.PreferredWidth(), S(110));
        _addButton.Bounds = new Rect(width - pad - addWidth, buttonY, addWidth, S(36));
        int cancelWidth = _cancel.PreferredWidth();
        _cancel.Bounds = new Rect(_addButton.Bounds.X - S(8) - cancelWidth, buttonY, cancelWidth, S(36));
        _selectAll.Bounds = new Rect(pad, buttonY, _selectAll.PreferredWidth(), S(36));
    }

    protected override void PaintWindow(Gfx g)
    {
        var right = _cancel.Bounds.X - S(12);
        var left = _selectAll.Bounds.Right + S(12);
        g.Text(_status, new Rect(left, _cancel.Bounds.Y, right - left, _cancel.Bounds.H), Fonts.Small, Theme.P.SubText);
    }

    private void Browse()
    {
        if (FileDialogs.OpenFolders(Handle, "选择搜索位置", multiple: false).FirstOrDefault() is not { } folder)
            return;
        _location.Value = folder;
        StartSearch();
    }

    private void StartSearch()
    {
        _stop?.Cancel();
        string root = _location.Value.Trim().Trim('"');
        s_lastLocation = root;
        lock (_gate)
            _found.Clear();
        _list.SetFiles([]);
        UpdateAddButton();
        if (!Directory.Exists(root))
        {
            _status = root.Length > 0 ? $"'{root}' isn't a folder here." : "";
            _searching = false;
            Invalidate();
            return;
        }

        var stop = new CancellationTokenSource();
        _stop = stop;
        _searching = true;
        _status = "正在搜索…";
        var worker = new Thread(() => Search(root, stop.Token)) { IsBackground = true, Name = "搜索" };
        worker.Start();
    }

    private void Search(string root, CancellationToken token)
    {
        var options = new EnumerationOptions
        {
            RecurseSubdirectories = true,
            IgnoreInaccessible = true,
            MatchType = MatchType.Simple,
            MatchCasing = MatchCasing.CaseInsensitive,
            AttributesToSkip = FileAttributes.ReparsePoint | FileAttributes.Offline,
        };
        try
        {
            foreach (string path in Directory.EnumerateFiles(root, "*", options))
            {
                if (token.IsCancellationRequested)
                    return;
                if (!GameArchive.IsGameFile(path))
                    continue;
                long size = 0;
                try
                {
                    size = new FileInfo(path).Length;
                }
                catch (IOException)
                {
                    continue;
                }
                bool compressed = Path.GetFileName(path).Contains("[LZ4]", StringComparison.OrdinalIgnoreCase);
                lock (_gate)
                    _found.Add(new FoundFile(path, size) { Chosen = !compressed });
                PostRefresh();
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            // Whatever was found until now is still useful.
        }
        if (token.IsCancellationRequested)
            return;
        _searching = false;
        PostRefresh();
    }

    private void PostRefresh()
    {
        if (!IsClosed && Interlocked.Exchange(ref _refreshPending, 1) == 0)
            PostFromWorker();
    }

    private void ApplyFilter()
    {
        string filter = _filter.Value.Trim();
        List<FoundFile> files;
        lock (_gate)
            files = _found.Where(f => filter.Length == 0 || f.Path.Contains(filter, StringComparison.OrdinalIgnoreCase)).ToList();
        _list.SetFiles(files);
        UpdateAddButton();
        Invalidate();
    }

    private void UpdateAddButton()
    {
        var visible = _list.Files;
        int chosen = visible.Count(f => f.Chosen);
        _addButton.Text = chosen == 1 ? "Add 1 file" : $"Add {chosen} files";
        _addButton.Enabled = chosen > 0;
        _selectAll.Text = visible.Count > 0 && chosen == visible.Count ? "取消全选" : "全选";
        long bytes = visible.Where(f => f.Chosen).Sum(f => f.Size);
        string found = $"{visible.Count} found";
        _status = (_searching ? "正在搜索…" : "") + found + (chosen > 0 ? $", {ProgressReporter.Format(bytes)} chosen" : "");
        Relayout();
    }

    private void ToggleAll()
    {
        var visible = _list.Files;
        bool all = visible.All(f => f.Chosen);
        foreach (var file in visible)
            file.Chosen = !all;
        UpdateAddButton();
    }

    private void AddChosen()
    {
        var chosen = _list.Files.Where(f => f.Chosen).Select(f => f.Path).ToList();
        if (chosen.Count == 0)
            return;
        _add(chosen);
        Close();
    }
}

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
